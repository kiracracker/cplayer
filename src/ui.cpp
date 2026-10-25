#include <raylib/raylib.h>
#include <utils.h>
#include <cover.h>
#include <taglib/fileref.h>
#include <mpeg/mpegfile.h>
#include <mpeg/id3v2/id3v2tag.h>
#include <mpeg/id3v2/frames/attachedpictureframe.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cctype>
#include <random>
#include <numeric>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <cstdio>

// curl.h pulls in <windows.h> on Windows (via winsock2.h) to support sockets.
// windows.h declares Rectangle/CloseWindow/ShowCursor/DrawTextEx too (from the
// GDI and USER subsystems), which collide head-on with raylib's identically
// named types/functions. NOGDI/NOUSER tell windows.h to skip those specific
// subsystems' declarations, since we don't use Win32 GDI/USER APIs directly
// (raylib/GLFW handles windowing on its own).
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOGDI
    #define NOUSER
#endif
#include <curl/curl.h>

#include <hb.h>
#include <hb-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H

// ----------------------------------------------------------------------------
// Minimal script-run splitter (a tiny stand-in for a full Unicode bidi
// algorithm). Forcing an entire mixed string (e.g. "Title 2023 | Arabic text")
// through one RTL shaping pass reverses digit order ("2023" -> "3202") because
// the whole buffer gets treated as one RTL run. Real bidi handling needs a
// library like FriBidi; this covers the common case (Arabic phrases with
// embedded Latin/digit fragments) by splitting into per-script runs, shaping
// each run on its own (so digits/Latin stay in their original order), then
// laying the runs out right-to-left if the paragraph is Arabic-dominant.
// ----------------------------------------------------------------------------
enum class CharClass { Arabic, Other, Neutral };

CharClass ClassifyCodepoint(uint32_t cp)
{
    if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
        (cp >= 0xFB50 && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF))
        return CharClass::Arabic;
    if ((cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z'))
        return CharClass::Other;
    return CharClass::Neutral; // spaces, punctuation, symbols
}

uint32_t DecodeUtf8At(const std::string& s, size_t& i)
{
    unsigned char c0 = (unsigned char)s[i];
    int len = 1;
    uint32_t cp = c0;
    if      ((c0 & 0x80) == 0x00) { len = 1; cp = c0; }
    else if ((c0 & 0xE0) == 0xC0) { len = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { len = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { len = 4; cp = c0 & 0x07; }
    for (int k = 1; k < len && (i + k) < s.size(); k++)
        cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    i += len;
    return cp;
}

struct TextRuns
{
    std::vector<std::string> runs; // in logical (reading) order
    bool isRtlParagraph = false;
};

TextRuns SplitIntoScriptRuns(const std::string& utf8)
{
    TextRuns result;
    std::string current;
    CharClass currentClass = CharClass::Neutral;
    bool haveClass = false;
    int arabicStrongCount = 0, otherStrongCount = 0;

    size_t i = 0;
    while (i < utf8.size())
    {
        size_t start = i;
        uint32_t cp = DecodeUtf8At(utf8, i);
        CharClass cls = ClassifyCodepoint(cp);
        if (cls == CharClass::Arabic) arabicStrongCount++;
        else if (cls == CharClass::Other) otherStrongCount++;

        if (cls == CharClass::Neutral)
        {
            // Spaces/punctuation attach to whatever run they're touching
            // instead of splintering into their own single-character runs.
            current.append(utf8, start, i - start);
            continue;
        }

        if (!haveClass) { currentClass = cls; haveClass = true; }
        else if (cls != currentClass)
        {
            result.runs.push_back(current);
            current.clear();
            currentClass = cls;
        }
        current.append(utf8, start, i - start);
    }
    if (!current.empty()) result.runs.push_back(current);

    result.isRtlParagraph = arabicStrongCount > 0 && arabicStrongCount >= otherStrongCount;
    return result;
}

// ============================================================================
// Arabic text shaping (HarfBuzz + FreeType)
//
// raylib's font system indexes glyphs by Unicode codepoint, which is fine for
// Latin text but wrong for Arabic: each Arabic letter has up to 4 different
// visual forms (isolated/initial/medial/final) depending on its neighbors,
// and letters must be joined into ligature-like connected shapes. That
// substitution logic lives in the font's GSUB table and only HarfBuzz (or an
// equivalent shaping engine) knows how to apply it.
//
// This shaper rasterizes each *shaped glyph* (by glyph index, via FreeType)
// once, caches the resulting texture, and positions successive glyphs using
// the pen offsets HarfBuzz computes. One instance is needed per font size,
// since glyph indices are rasterized at a fixed pixel size.
// ============================================================================
struct CachedGlyph
{
    Texture2D tex = {0};
    int bitmapLeft = 0;
    int bitmapTop  = 0;
};

struct ArabicShaper
{
    FT_Library ftLibrary = nullptr;
    FT_Face    ftFace    = nullptr;
    hb_font_t* hbFont    = nullptr;
    std::unordered_map<unsigned int, CachedGlyph> glyphCache;

    bool Init(const char* fontPath, int pixelSize)
    {
        if (FT_Init_FreeType(&ftLibrary) != 0) return false;
        if (FT_New_Face(ftLibrary, fontPath, 0, &ftFace) != 0) return false;
        FT_Set_Pixel_Sizes(ftFace, 0, pixelSize);

        hbFont = hb_ft_font_create(ftFace, nullptr);
        return hbFont != nullptr;
    }

    void Shutdown()
    {
        for (auto& kv : glyphCache)
            if (kv.second.tex.id != 0) UnloadTexture(kv.second.tex);
        glyphCache.clear();

        if (hbFont)    hb_font_destroy(hbFont);
        if (ftFace)    FT_Done_Face(ftFace);
        if (ftLibrary) FT_Done_FreeType(ftLibrary);
    }

    const CachedGlyph& GetGlyph(unsigned int glyphIndex)
    {
        auto it = glyphCache.find(glyphIndex);
        if (it != glyphCache.end()) return it->second;

        CachedGlyph g;
        if (FT_Load_Glyph(ftFace, glyphIndex, FT_LOAD_RENDER) == 0)
        {
            FT_GlyphSlot slot = ftFace->glyph;
            FT_Bitmap& bmp = slot->bitmap;
            g.bitmapLeft = slot->bitmap_left;
            g.bitmapTop  = slot->bitmap_top;

            if (bmp.width > 0 && bmp.rows > 0)
            {
                // FreeType gives 8-bit coverage; turn it into a white RGBA
                // texture (alpha = coverage) so DrawTexture's tint colors it.
                std::vector<unsigned char> pixels((size_t)bmp.width * bmp.rows * 4);
                for (unsigned int y = 0; y < bmp.rows; y++)
                {
                    for (unsigned int x = 0; x < bmp.width; x++)
                    {
                        unsigned char a = bmp.buffer[y * bmp.pitch + x];
                        size_t idx = ((size_t)y * bmp.width + x) * 4;
                        pixels[idx + 0] = 255;
                        pixels[idx + 1] = 255;
                        pixels[idx + 2] = 255;
                        pixels[idx + 3] = a;
                    }
                }
                Image img = { pixels.data(), (int)bmp.width, (int)bmp.rows, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
                g.tex = LoadTextureFromImage(img);
                SetTextureFilter(g.tex, TEXTURE_FILTER_BILINEAR);
            }
        }

        auto inserted = glyphCache.emplace(glyphIndex, g);
        return inserted.first->second;
    }

    // Measures shaped width in pixels, run-by-run (see SplitIntoScriptRuns).
    float MeasureText(const std::string& utf8Text)
    {
        TextRuns runs = SplitIntoScriptRuns(utf8Text);
        float total = 0.0f;
        for (const std::string& run : runs.runs)
            total += ShapeRunWidth(run);
        return total;
    }

    // Draws shaped text. `topLeft` is the left edge of the string's bounding box,
    // matching how DrawTextEx's position parameter is used elsewhere in this file.
    // Runs are laid out right-to-left when the paragraph is Arabic-dominant, but
    // each run's own characters (e.g. a digit sequence) keep their original order.
    void DrawText(const std::string& utf8Text, Vector2 topLeft, Color tint)
    {
        TextRuns runs = SplitIntoScriptRuns(utf8Text);
        std::vector<std::string> order = runs.runs;
        if (runs.isRtlParagraph) std::reverse(order.begin(), order.end());

        float ascenderPx = ftFace->size->metrics.ascender / 64.0f;
        float cursorX = topLeft.x;
        float baselineY = topLeft.y + ascenderPx;

        for (const std::string& run : order)
            cursorX += DrawRun(run, cursorX, baselineY, tint);
    }

private:
    float ShapeRunWidth(const std::string& run)
    {
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, run.c_str(), -1, 0, -1);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(hbFont, buf, nullptr, 0);

        unsigned int glyphCount = 0;
        hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(buf, &glyphCount);
        float total = 0.0f;
        for (unsigned int i = 0; i < glyphCount; i++) total += pos[i].x_advance / 64.0f;

        hb_buffer_destroy(buf);
        return total;
    }

    // Draws one run starting at (startX, baselineY), returns its advance width.
    float DrawRun(const std::string& run, float startX, float baselineY, Color tint)
    {
        hb_buffer_t* buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, run.c_str(), -1, 0, -1);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(hbFont, buf, nullptr, 0);

        unsigned int glyphCount = 0;
        hb_glyph_info_t*     infos = hb_buffer_get_glyph_infos(buf, &glyphCount);
        hb_glyph_position_t* pos   = hb_buffer_get_glyph_positions(buf, &glyphCount);

        float cursorX = startX;
        float cursorY = baselineY;
        for (unsigned int i = 0; i < glyphCount; i++)
        {
            const CachedGlyph& g = GetGlyph(infos[i].codepoint);
            if (g.tex.id != 0)
            {
                float gx = cursorX + (pos[i].x_offset / 64.0f) + g.bitmapLeft;
                float gy = cursorY - (pos[i].y_offset / 64.0f) - g.bitmapTop;
                DrawTexture(g.tex, (int)gx, (int)gy, tint);
            }
            cursorX += pos[i].x_advance / 64.0f;
            cursorY += pos[i].y_advance / 64.0f;
        }

        hb_buffer_destroy(buf);
        return cursorX - startX;
    }

public:
};

// One shaper per font size actually used (glyph rasterization is size-specific).
ArabicShaper gShaperTitle;
ArabicShaper gShaperArtist;

// Default LoadFontEx only pulls in ASCII glyphs, which is why Arabic text
// rendered as "?????". Build an explicit codepoint list covering basic Latin
// plus the Arabic Unicode blocks so titles/artists in Arabic actually render.
std::vector<int> BuildFontCodepoints()
{
    std::vector<int> cps;
    for (int c = 0x0020; c <= 0x007E; c++) cps.push_back(c); // basic Latin + punctuation/digits
    for (int c = 0x0600; c <= 0x06FF; c++) cps.push_back(c); // Arabic
    for (int c = 0x0750; c <= 0x077F; c++) cps.push_back(c); // Arabic Supplement
    for (int c = 0xFE70; c <= 0xFEFF; c++) cps.push_back(c); // Arabic Presentation Forms-B (isolated/joined shapes)
    return cps;
}

const int screenWidth  = 1280;
const int screenHeight = 720;

// ---- Spotify-like palette ----
const Color COL_BG          = {18, 18, 18, 255};   // main background
const Color COL_BAR_BG      = {24, 24, 24, 255};   // bottom player bar
const Color COL_GREEN       = {30, 215, 96, 255};  // spotify green
const Color COL_TRACK_BG    = {70, 70, 70, 255};   // progress bar track
const Color COL_TEXT_WHITE  = {255, 255, 255, 255};
const Color COL_TEXT_GRAY   = {179, 179, 179, 255};
const Color COL_ICON_IDLE   = {200, 200, 200, 255};

// Small helper since raylib.h alone (without raymath.h) doesn't declare Clamp().
float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

Music    music;
Texture2D cover = {0};

std::string trackTitle  = "No track loaded";
std::string trackArtist = "Drop an mp3 file here";

std::vector<std::string> playlist;
int currentTrackIndex = -1;
float gVolume = 0.7f;
float gVolumeBeforeMute = 0.7f; // remembered so the mute toggle can restore it
bool pause = false;

enum class RepeatMode { Off, All, One };
RepeatMode repeatMode = RepeatMode::Off;

bool shuffleOn = false;
std::vector<int> shuffleOrder; // permutation of playlist indices, only valid while shuffleOn
int shufflePos = 0;            // position within shuffleOrder of the currently playing track

// Draws an icon scaled to fit inside a target size and centered on `center`,
// with an optional horizontal nudge for visually-asymmetric icons (like a play triangle).
void DrawIconFit(Texture2D icon, Vector2 center, float maxSize, Color tint, float offsetX = 0.0f)
{
    if (icon.id == 0) return;
    float longest = (float)((icon.width > icon.height) ? icon.width : icon.height);
    float scale = maxSize / longest;
    float w = icon.width  * scale;
    float h = icon.height * scale;

    Rectangle src = { 0, 0, (float)icon.width, (float)icon.height };
    Rectangle dst = { center.x - w / 2 + offsetX, center.y - h / 2, w, h };
    DrawTexturePro(icon, src, dst, { 0, 0 }, 0, tint);
}

// Truncates a UTF-8 string to at most maxChars codepoints, appending "..." if cut.
std::string TruncateUtf8(const std::string& s, size_t maxChars)
{
    size_t i = 0, count = 0;
    while (i < s.size() && count < maxChars)
    {
        unsigned char c = (unsigned char)s[i];
        int len = 1;
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
        count++;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "...";
}

// ---- .m3u / .m3u8 playlist file support ----
bool HasExtension(const std::string& path, const std::string& extLower)
{
    if (path.size() < extLower.size() + 1) return false;
    std::string tail = path.substr(path.size() - extLower.size());
    for (auto& c : tail) c = (char)tolower((unsigned char)c);
    return tail == extLower;
}

std::string GetDirectory(const std::string& path)
{
    size_t pos = path.find_last_of("/\\");
    return (pos == std::string::npos) ? "" : path.substr(0, pos + 1);
}

bool IsAbsolutePath(const std::string& path)
{
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
    if (path.size() > 1 && path[1] == ':') return true; // e.g. "C:\..."
    return false;
}

// Trims stray whitespace (common when pasting) and adds a scheme if the user
// typed a bare domain like "example.com/song.mp3" — curl needs one, and
// without this the download just fails with a confusing error.
std::string NormalizeUrl(const std::string& raw)
{
    size_t start = raw.find_first_not_of(" \t\r\n");
    size_t end   = raw.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    std::string url = raw.substr(start, end - start + 1);

    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0)
        url = "https://" + url;
    return url;
}

// Parses an m3u/m3u8 file into a list of track paths. Lines starting with '#'
// are metadata (#EXTM3U, #EXTINF, ...) and are skipped; relative paths are
// resolved against the .m3u file's own directory, since that's what entries
// in a playlist file are normally relative to.
std::vector<std::string> ParseM3U(const std::string& m3uPath)
{
    std::vector<std::string> tracks;
    std::ifstream file(m3uPath);
    if (!file.is_open()) return tracks;

    std::string dir = GetDirectory(m3uPath);
    std::string line;
    while (std::getline(file, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // CRLF files
        if (line.empty() || line[0] == '#') continue;

        tracks.push_back(IsAbsolutePath(line) ? line : dir + line);
    }
    return tracks;
}

// ============================================================================
// Online URL playback: raylib/LoadMusicStream only reads local files, so we
// download the URL to a temp file first (via libcurl, on a background thread
// so the UI doesn't freeze), then load that temp file exactly like a dropped
// one. This covers direct audio links and podcast-style URLs; it is NOT a
// live-streaming implementation (no partial playback while downloading).
// ============================================================================
struct DownloadState
{
    std::mutex mtx;
    bool inProgress = false;
    bool done = false;   // one-shot flag, consumed by the main thread each frame
    bool failed = false;
    std::string resultPath;
    std::string errorMsg;
};
DownloadState gDownload;
std::thread   gDownloadThread;

std::string GetTempDir()
{
#ifdef _WIN32
    const char* t = std::getenv("TEMP");
    if (!t) t = std::getenv("TMP");
    return t ? (std::string(t) + "\\") : ".\\";
#else
    return "/tmp/";
#endif
}

size_t CurlWriteToFile(void* contents, size_t size, size_t nmemb, void* userp)
{
    FILE* f = (FILE*)userp;
    return fwrite(contents, size, nmemb, f);
}

// Runs on a worker thread (see gDownloadThread). Downloads `url` to a temp
// file and reports the result via gDownload (mutex-guarded).
void DownloadUrlThread(std::string url)
{
    {
        std::lock_guard<std::mutex> lock(gDownload.mtx);
        gDownload.inProgress = true;
        gDownload.done = false;
        gDownload.failed = false;
    }

    // Keep the URL's extension if it looks like a normal audio file extension,
    // so the loader's format sniffing (by extension) still works; default to .mp3.
    std::string ext = ".mp3";
    size_t dot = url.find_last_of('.');
    if (dot != std::string::npos)
    {
        std::string maybeExt = url.substr(dot);
        size_t cut = maybeExt.find_first_of("?#");
        if (cut != std::string::npos) maybeExt = maybeExt.substr(0, cut);
        if (maybeExt.size() >= 2 && maybeExt.size() <= 5) ext = maybeExt;
    }

    long long nowMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string tempPath = GetTempDir() + "musicplayer_dl_" + std::to_string(nowMs) + ext;

    bool ok = false;
    std::string error;

    FILE* f = fopen(tempPath.c_str(), "wb");
    if (!f)
    {
        error = "Could not create temp file";
    }
    else
    {
        CURL* curl = curl_easy_init();
        if (curl)
        {
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToFile);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, "MusicPlayer/1.0");

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

            if (res != CURLE_OK) error = curl_easy_strerror(res);
            else if (httpCode >= 400) error = "HTTP " + std::to_string(httpCode);
            else ok = true;

            curl_easy_cleanup(curl);
        }
        else
        {
            error = "curl_easy_init failed";
        }
        fclose(f);
    }

    if (!ok && !tempPath.empty()) remove(tempPath.c_str());

    std::lock_guard<std::mutex> lock(gDownload.mtx);
    gDownload.inProgress = false;
    gDownload.done = true;
    gDownload.failed = !ok;
    gDownload.resultPath = ok ? tempPath : "";
    gDownload.errorMsg = error;
}

void LoadTrackMeta(const char* path)
{
    TagLib::FileRef ref(path);
    if (!ref.isNull() && ref.tag())
    {
        TagLib::Tag* tag = ref.tag();
        std::string title  = tag->title().toCString(true);
        std::string artist = tag->artist().toCString(true);
        trackTitle  = TruncateUtf8(title.empty()  ? "Unknown title"  : title, 60);
        trackArtist = TruncateUtf8(artist.empty() ? "Unknown artist" : artist, 45);
    }
}

// Loads the playlist entry at `index` (wrapping around at both ends, so skip
// past the last track loops to the first, and back past the first loops to
// the last — standard player behavior).
void LoadTrackAtIndex(int index)
{
    if (playlist.empty()) return;
    if (index < 0) index = (int)playlist.size() - 1;
    if (index >= (int)playlist.size()) index = 0;
    currentTrackIndex = index;

    const std::string& path = playlist[currentTrackIndex];

    if (cover.id != 0) { UnloadTexture(cover); cover = {0}; }
    cover = LoadCover(path.c_str());
    if (cover.id != 0) SetTextureFilter(cover, TEXTURE_FILTER_BILINEAR);

    LoadTrackMeta(path.c_str());

    if (music.ctxData != nullptr) UnloadMusicStream(music);
    music = LoadMusicStream(path.c_str());
    music.looping = false; // we handle end-of-track ourselves (auto-advance / repeat)
    SetMusicVolume(music, gVolume);
    PlayMusicStream(music);

    SetWindowTitle(TextFormat("%s - Music Player", trackTitle.c_str()));
}

// Rebuilds the shuffled play order. Keeps the currently playing track at the
// front of the shuffled sequence so toggling shuffle mid-song doesn't jump
// anywhere; only the *upcoming* order becomes randomized.
void RebuildShuffleOrder()
{
    shuffleOrder.resize(playlist.size());
    std::iota(shuffleOrder.begin(), shuffleOrder.end(), 0);

    static std::mt19937 rng(std::random_device{}());
    std::shuffle(shuffleOrder.begin(), shuffleOrder.end(), rng);

    auto it = std::find(shuffleOrder.begin(), shuffleOrder.end(), currentTrackIndex);
    if (it != shuffleOrder.end()) std::iter_swap(shuffleOrder.begin(), it);
    shufflePos = 0;
}

// Moves forward (+1) or backward (-1) through the playlist, respecting shuffle mode.
void AdvanceTrack(int direction)
{
    if (playlist.empty()) return;

    if (shuffleOn)
    {
        if (shuffleOrder.size() != playlist.size()) RebuildShuffleOrder();
        shufflePos += direction;
        if (shufflePos < 0) shufflePos = (int)shuffleOrder.size() - 1;
        if (shufflePos >= (int)shuffleOrder.size()) shufflePos = 0;
        LoadTrackAtIndex(shuffleOrder[shufflePos]);
    }
    else
    {
        LoadTrackAtIndex(currentTrackIndex + direction);
    }
}

// Called once when the current track naturally reaches its end.
void HandleTrackEnd()
{
    if (repeatMode == RepeatMode::One)
    {
        PlayMusicStream(music); // restarts cleanly even after the stream has fully stopped
        return;
    }

    bool atLastTrack = !shuffleOn && (currentTrackIndex + 1 >= (int)playlist.size());
    if (repeatMode == RepeatMode::Off && atLastTrack)
    {
        // End of a non-repeating, non-shuffled queue: stop instead of wrapping.
        SeekMusicStream(music, 0.0f);
        PauseMusicStream(music);
        pause = true;
        return;
    }

    AdvanceTrack(+1);
    pause = false;
}

// ============================================================================
// mpv-style command console. Press ` (backtick) to open, type a command,
// Enter to run it, Escape to cancel. Supported commands:
//   play | pause | toggle
//   next | n            prev | previous | p
//   seek <sec>          seek +<sec> | seek -<sec>   (relative)
//   volume <0-100>
//   shuffle
//   repeat <off|all|one>   (no arg = cycle, same as the button)
//   load <path-or-url>     (.m3u/.m3u8 expands, http(s):// downloads)
//   quit | q
// ============================================================================
bool requestQuit = false;

void ExecuteConsoleCommand(const std::string& line)
{
    std::istringstream iss(line);
    std::string cmd;
    iss >> cmd;
    for (auto& c : cmd) c = (char)tolower((unsigned char)c);

    if (cmd == "play")
    {
        pause = false;
        if (music.ctxData != nullptr) ResumeMusicStream(music);
    }
    else if (cmd == "pause")
    {
        pause = true;
        if (music.ctxData != nullptr) PauseMusicStream(music);
    }
    else if (cmd == "toggle" && music.ctxData != nullptr)
    {
        pause = !pause;
        if (pause) PauseMusicStream(music); else ResumeMusicStream(music);
    }
    else if ((cmd == "next" || cmd == "n") && !playlist.empty())
    {
        AdvanceTrack(+1);
        pause = false;
    }
    else if ((cmd == "prev" || cmd == "previous" || cmd == "p") && !playlist.empty())
    {
        float playedNow = (music.ctxData != nullptr) ? GetMusicTimePlayed(music) : 0.0f;
        if (playedNow > 3.0f) SeekMusicStream(music, 0.0f);
        else AdvanceTrack(-1);
        pause = false;
    }
    else if (cmd == "seek" && music.ctxData != nullptr)
    {
        std::string arg;
        iss >> arg;
        if (!arg.empty())
        {
            try
            {
                float len = GetMusicTimeLength(music);
                float cur = GetMusicTimePlayed(music);
                float target = (arg[0] == '+' || arg[0] == '-') ? cur + std::stof(arg) : std::stof(arg);
                target = ClampF(target, 0.0f, len);
                SeekMusicStream(music, target);
            }
            catch (...) { /* ignore malformed number */ }
        }
    }
    else if (cmd == "volume" || cmd == "vol")
    {
        std::string arg;
        iss >> arg;
        if (!arg.empty())
        {
            try { gVolume = ClampF(std::stof(arg) / 100.0f, 0.0f, 1.0f); }
            catch (...) { /* ignore malformed number */ }
        }
    }
    else if (cmd == "shuffle")
    {
        shuffleOn = !shuffleOn;
        if (shuffleOn) RebuildShuffleOrder();
    }
    else if (cmd == "repeat")
    {
        std::string arg;
        iss >> arg;
        for (auto& c : arg) c = (char)tolower((unsigned char)c);
        if      (arg == "off") repeatMode = RepeatMode::Off;
        else if (arg == "all") repeatMode = RepeatMode::All;
        else if (arg == "one" || arg == "1") repeatMode = RepeatMode::One;
        else // no/unknown arg: cycle, same as clicking the button
        {
            if      (repeatMode == RepeatMode::Off) repeatMode = RepeatMode::All;
            else if (repeatMode == RepeatMode::All) repeatMode = RepeatMode::One;
            else                                     repeatMode = RepeatMode::Off;
        }
    }
    else if (cmd == "load")
    {
        std::string rest;
        std::getline(iss, rest);
        size_t s = rest.find_first_not_of(' ');
        if (s != std::string::npos) rest = rest.substr(s);

        if (!rest.empty())
        {
            if (rest.rfind("http://", 0) == 0 || rest.rfind("https://", 0) == 0)
            {
                if (gDownloadThread.joinable()) gDownloadThread.join();
                gDownloadThread = std::thread(DownloadUrlThread, NormalizeUrl(rest));
            }
            else if (HasExtension(rest, "m3u") || HasExtension(rest, "m3u8"))
            {
                std::vector<std::string> tracks = ParseM3U(rest);
                if (!tracks.empty())
                {
                    playlist = tracks;
                    LoadTrackAtIndex(0);
                    pause = false;
                }
            }
            else
            {
                playlist.push_back(rest);
                LoadTrackAtIndex((int)playlist.size() - 1);
                pause = false;
            }
        }
    }
    else if (cmd == "quit" || cmd == "q")
    {
        requestQuit = true;
    }
}

void DrawUI()
{
    // HighDPI stops Windows from bitmap-upscaling the window when display
    // scaling is on (the main source of the "everything looks pixelated"
    // look). MSAA smooths the edges of the rounded rects/circles we draw.
    // RESIZABLE lets the user resize the window; ALWAYS_RUN keeps the loop
    // (and therefore audio playback) going while minimized or unfocused,
    // which raylib otherwise throttles/pauses on some platforms.
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN);
    InitWindow(screenWidth, screenHeight, "Music Player");
    SetWindowMinSize(720, 480); // below this, controls can no longer fit without overlapping

    // The .rc-embedded icon only covers the exe file's icon in Explorer/shortcuts.
    // The *running* window's taskbar + title-bar icon is a separate thing that
    // has to be set explicitly at runtime, or it falls back to GLFW's default.
    Image iconImg = LoadImage("../icons/app_icon.png");
    if (iconImg.data != nullptr)
    {
        SetWindowIcon(iconImg);
        UnloadImage(iconImg);
    }

    // A bigger audio buffer trades a little latency for a lot of stability —
    // it gives the audio thread more headroom before it runs out of queued
    // samples (which is what causes crackling/stutter), especially during
    // brief main-thread stalls (window drag/resize, heavy frame). The extra
    // latency (well under 100ms) is not noticeable for music playback.
    SetAudioStreamBufferSizeDefault(4096);
    InitAudioDevice();
    SetTargetFPS(60);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Texture2D pauseIcon    = LoadImageTexture("../icons/puse.png");
    Texture2D playIcon     = LoadImageTexture("../icons/play.png");
    Texture2D skipIcon     = LoadImageTexture("../icons/skip.png");
    Texture2D backwardIcon = LoadImageTexture("../icons/backward.png");
    SetTextureFilter(pauseIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(playIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(skipIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(backwardIcon, TEXTURE_FILTER_BILINEAR);

    std::vector<int> fontCodepoints = BuildFontCodepoints();
    Font gFont      = LoadFontEx("../fonts/NotoSansArabic-Black.ttf", 34,
                                  fontCodepoints.data(), (int)fontCodepoints.size());
    Font gFontSmall = LoadFontEx("../fonts/NotoSansArabic-Black.ttf", 22,
                                  fontCodepoints.data(), (int)fontCodepoints.size());
    SetTextureFilter(gFont.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(gFontSmall.texture, TEXTURE_FILTER_BILINEAR);

    // HarfBuzz shapers for Arabic title/artist text (one per pixel size).
    bool shaperOk = gShaperTitle.Init("../fonts/NotoSansArabic-Black.ttf", 34)
                 && gShaperArtist.Init("../fonts/NotoSansArabic-Black.ttf", 22);
    if (!shaperOk) TraceLog(LOG_WARNING, "Arabic shaper failed to init, falling back to raylib text");

    float timePlayed     = 0.0f;
    float minutesPlayed  = 0.0f;
    float minutesTotal   = 0.0f;

    // ---- Layout is recomputed every frame from the live window size (see
    // inside the loop below) so resizing the window doesn't break it ----
    bool draggingSeek = false;
    bool draggingVolume = false;

    // ---- Online URL input box state ----
    char urlBuffer[512] = "";
    int  urlBufferLen = 0;
    bool urlBoxActive = false;
    std::string urlStatusMsg;      // shows "Downloading..." or an error
    bool urlStatusIsError = false;

    // ---- mpv-style command console state ----
    char consoleBuffer[256] = "";
    int  consoleBufferLen = 0;
    bool consoleActive = false;

    while (!WindowShouldClose() && !requestQuit)
    {
        // ---- Layout, recomputed from the *live* window size every frame ----
        float sw = (float)GetScreenWidth();
        float sh = (float)GetScreenHeight();

        float coverSize = ClampF(fminf(sw, sh) * 0.35f, 120.0f, 280.0f);
        Rectangle coverDest = { (sw - coverSize) / 2.0f, 70, coverSize, coverSize };

        const float barHeight = 110;
        Rectangle playerBar = { 0, sh - barHeight, sw, barHeight };

        float progressW = ClampF(sw - 200.0f, 160.0f, 640.0f);
        Rectangle progressBg = { (sw - progressW) / 2.0f, playerBar.y + 22, progressW, 5 };

        Vector2 playBtnCenter     = { sw / 2.0f, playerBar.y + 66 };
        Vector2 backwardBtnCenter = { playBtnCenter.x - 80, playBtnCenter.y };
        Vector2 skipBtnCenter     = { playBtnCenter.x + 80, playBtnCenter.y };
        Vector2 shuffleBtnCenter  = { backwardBtnCenter.x - 55, playBtnCenter.y };
        Vector2 repeatBtnCenter   = { skipBtnCenter.x + 55, playBtnCenter.y };
        const float playBtnRadius  = 24;
        const float sideBtnRadius  = 18;
        const float smallBtnRadius = 14;

        // Volume control: small speaker glyph + slider, bottom-right of the bar.
        const float volumeW = 90;
        Rectangle volumeBg = { sw - volumeW - 40, playerBar.y + 66 - 2, volumeW, 4 };
        Vector2 speakerCenter = { volumeBg.x - 22, volumeBg.y + 2 };

        // Online URL input, top of the window.
        Rectangle urlBox = { sw / 2.0f - 260, 16, 520, 32 };

        // ---- Handle file drop (supports dropping multiple files, and .m3u/.m3u8 playlists) ----
        if (IsFileDropped())
        {
            FilePathList files = LoadDroppedFiles();

            std::vector<std::string> newPlaylist;
            for (unsigned int i = 0; i < files.count; i++)
            {
                std::string path = files.paths[i];
                if (HasExtension(path, "m3u") || HasExtension(path, "m3u8"))
                {
                    std::vector<std::string> tracks = ParseM3U(path);
                    newPlaylist.insert(newPlaylist.end(), tracks.begin(), tracks.end());
                }
                else
                {
                    newPlaylist.push_back(path);
                }
            }

            UnloadDroppedFiles(files);

            if (!newPlaylist.empty())
            {
                playlist = newPlaylist;
                LoadTrackAtIndex(0);
                pause = false;
            }
        }

        // ---- Command console: ` toggles it open/closed, Escape cancels ----
        bool justOpenedConsole = false;
        if (IsKeyPressed(KEY_GRAVE))
        {
            consoleActive = !consoleActive;
            if (consoleActive) { urlBoxActive = false; justOpenedConsole = true; }
        }
        if (justOpenedConsole)
        {
            while (GetCharPressed() > 0) {} // discard the backtick itself from the char queue
        }
        if (consoleActive && IsKeyPressed(KEY_ESCAPE))
        {
            consoleActive = false;
            consoleBuffer[0] = '\0';
            consoleBufferLen = 0;
        }
        if (consoleActive && !justOpenedConsole)
        {
            int key = GetCharPressed();
            while (key > 0)
            {
                if (key >= 32 && key < 127 && consoleBufferLen < (int)sizeof(consoleBuffer) - 1)
                {
                    consoleBuffer[consoleBufferLen++] = (char)key;
                    consoleBuffer[consoleBufferLen] = '\0';
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && consoleBufferLen > 0)
            {
                consoleBufferLen--;
                consoleBuffer[consoleBufferLen] = '\0';
            }
            bool ctrlDownConsole = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (ctrlDownConsole && IsKeyPressed(KEY_V))
            {
                const char* clip = GetClipboardText();
                if (clip != nullptr)
                {
                    for (const char* p = clip; *p != '\0' && consoleBufferLen < (int)sizeof(consoleBuffer) - 1; p++)
                    {
                        if (*p == '\r' || *p == '\n' || *p == '\t') continue;
                        consoleBuffer[consoleBufferLen++] = *p;
                    }
                    consoleBuffer[consoleBufferLen] = '\0';
                }
            }
            if (IsKeyPressed(KEY_ENTER) && consoleBufferLen > 0)
            {
                ExecuteConsoleCommand(std::string(consoleBuffer));
                consoleBuffer[0] = '\0';
                consoleBufferLen = 0;
                consoleActive = false; // mpv also closes the console after Enter
            }
        }

        // ---- Online URL box: click to focus, type a direct audio URL, Enter to load ----
        {
            Vector2 mouseForUrl = GetMousePosition();
            bool hoverUrlBox = CheckCollisionPointRec(mouseForUrl, urlBox);
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            {
                urlBoxActive = hoverUrlBox;
                if (urlBoxActive) consoleActive = false;
            }

            if (urlBoxActive)
            {
                int key = GetCharPressed();
                while (key > 0)
                {
                    if (key >= 32 && key < 127 && urlBufferLen < (int)sizeof(urlBuffer) - 1)
                    {
                        urlBuffer[urlBufferLen++] = (char)key;
                        urlBuffer[urlBufferLen] = '\0';
                        urlStatusMsg.clear();
                    }
                    key = GetCharPressed();
                }
                if (IsKeyPressed(KEY_BACKSPACE) && urlBufferLen > 0)
                {
                    urlBufferLen--;
                    urlBuffer[urlBufferLen] = '\0';
                    urlStatusMsg.clear();
                }

                // Ctrl+V paste — GetCharPressed() above doesn't fire for clipboard
                // paste, so it needs its own handling via GetClipboardText().
                bool ctrlDown = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (ctrlDown && IsKeyPressed(KEY_V))
                {
                    const char* clip = GetClipboardText();
                    if (clip != nullptr)
                    {
                        for (const char* p = clip; *p != '\0' && urlBufferLen < (int)sizeof(urlBuffer) - 1; p++)
                        {
                            if (*p == '\r' || *p == '\n' || *p == '\t') continue; // strip stray whitespace
                            urlBuffer[urlBufferLen++] = *p;
                        }
                        urlBuffer[urlBufferLen] = '\0';
                        urlStatusMsg.clear();
                    }
                }
                if (IsKeyPressed(KEY_ENTER) && urlBufferLen > 0)
                {
                    bool alreadyRunning = false;
                    { std::lock_guard<std::mutex> lock(gDownload.mtx); alreadyRunning = gDownload.inProgress; }
                    if (!alreadyRunning)
                    {
                        if (gDownloadThread.joinable()) gDownloadThread.join();
                        urlStatusMsg = "Downloading...";
                        urlStatusIsError = false;
                        gDownloadThread = std::thread(DownloadUrlThread, NormalizeUrl(std::string(urlBuffer)));
                    }
                }
            }
        }

        // ---- Poll the background download for a result ----
        {
            std::lock_guard<std::mutex> lock(gDownload.mtx);
            if (gDownload.done)
            {
                gDownload.done = false;
                if (!gDownload.failed)
                {
                    playlist.push_back(gDownload.resultPath);
                    LoadTrackAtIndex((int)playlist.size() - 1);
                    pause = false;
                    urlBuffer[0] = '\0';
                    urlBufferLen = 0;
                    urlStatusMsg.clear();
                }
                else
                {
                    urlStatusMsg = gDownload.errorMsg.empty() ? "Download failed" : gDownload.errorMsg;
                    urlStatusIsError = true;
                }
            }
        }

        // ---- Input ----
        Vector2 mouse = GetMousePosition();
        bool hoverPlay     = CheckCollisionPointCircle(mouse, playBtnCenter, playBtnRadius);
        bool hoverBackward = CheckCollisionPointCircle(mouse, backwardBtnCenter, sideBtnRadius);
        bool hoverSkip     = CheckCollisionPointCircle(mouse, skipBtnCenter, sideBtnRadius);
        bool hoverShuffle  = CheckCollisionPointCircle(mouse, shuffleBtnCenter, smallBtnRadius);
        bool hoverRepeat   = CheckCollisionPointCircle(mouse, repeatBtnCenter, smallBtnRadius);
        Rectangle progressHitbox = { progressBg.x - 4, progressBg.y - 8, progressBg.width + 8, progressBg.height + 16 };
        bool hoverProgress = CheckCollisionPointRec(mouse, progressHitbox);
        Rectangle volumeHitbox = { volumeBg.x - 4, volumeBg.y - 8, volumeBg.width + 8, volumeBg.height + 16 };
        bool hoverVolume = CheckCollisionPointRec(mouse, volumeHitbox);
        bool hoverSpeaker = CheckCollisionPointCircle(mouse, speakerCenter, 12);

        if (hoverPlay && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && music.ctxData != nullptr)
        {
            pause = !pause;
            if (pause) PauseMusicStream(music);
            else       ResumeMusicStream(music);
        }

        if (hoverSkip && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !playlist.empty())
        {
            AdvanceTrack(+1);
            pause = false;
        }

        if (hoverBackward && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !playlist.empty())
        {
            // Spotify-style behavior: restart the current track if we're more
            // than 3s in, otherwise actually go back to the previous track.
            float playedNow = (music.ctxData != nullptr) ? GetMusicTimePlayed(music) : 0.0f;
            if (playedNow > 3.0f) SeekMusicStream(music, 0.0f);
            else AdvanceTrack(-1);
            pause = false;
        }

        if (hoverShuffle && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            shuffleOn = !shuffleOn;
            if (shuffleOn) RebuildShuffleOrder();
        }

        if (hoverRepeat && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            // cycle: Off -> All -> One -> Off
            if      (repeatMode == RepeatMode::Off) repeatMode = RepeatMode::All;
            else if (repeatMode == RepeatMode::All) repeatMode = RepeatMode::One;
            else                                     repeatMode = RepeatMode::Off;
        }

        if (hoverSpeaker && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        {
            // Mute toggle: remember the volume so a second click restores it.
            if (gVolume > 0.0f) { gVolumeBeforeMute = gVolume; gVolume = 0.0f; }
            else                 { gVolume = gVolumeBeforeMute > 0.0f ? gVolumeBeforeMute : 0.7f; }
        }

        // ---- Keyboard shortcuts (suppressed while typing in the URL box or console) ----
        if (!urlBoxActive && !consoleActive)
        {
        if (IsKeyPressed(KEY_SPACE) && music.ctxData != nullptr)
        {
            pause = !pause;
            if (pause) PauseMusicStream(music);
            else       ResumeMusicStream(music);
        }
        if (IsKeyPressed(KEY_N) && !playlist.empty()) { AdvanceTrack(+1); pause = false; }
        if (IsKeyPressed(KEY_P) && !playlist.empty())
        {
            float playedNow = (music.ctxData != nullptr) ? GetMusicTimePlayed(music) : 0.0f;
            if (playedNow > 3.0f) SeekMusicStream(music, 0.0f);
            else AdvanceTrack(-1);
            pause = false;
        }
        if (IsKeyPressed(KEY_UP))   gVolume = (gVolume + 0.05f > 1.0f) ? 1.0f : gVolume + 0.05f;
        if (IsKeyPressed(KEY_DOWN)) gVolume = (gVolume - 0.05f < 0.0f) ? 0.0f : gVolume - 0.05f;
        if (music.ctxData != nullptr && (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_LEFT)))
        {
            float cur = GetMusicTimePlayed(music);
            float len = GetMusicTimeLength(music);
            float target = cur + (IsKeyPressed(KEY_RIGHT) ? 5.0f : -5.0f);
            if (target < 0.0f) target = 0.0f;
            if (target > len)  target = len;
            SeekMusicStream(music, target);
        }
        } // end !urlBoxActive && !consoleActive guard

        if (hoverVolume && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) draggingVolume = true;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) draggingVolume = false;
        if (draggingVolume)
        {
            float ratio = (mouse.x - volumeBg.x) / volumeBg.width;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            gVolume = ratio;
        }
        if (music.ctxData != nullptr) SetMusicVolume(music, gVolume);

        // ---- Update playback progress (guarded against div-by-zero) ----
        float lengthSec = (music.ctxData != nullptr) ? GetMusicTimeLength(music) : 0.0f;
        float playedSec = (music.ctxData != nullptr) ? GetMusicTimePlayed(music) : 0.0f;

        // Track finished naturally (music.looping is off, so the stream stops
        // playing on its own at the end) -> auto-advance / repeat.
        //
        // IMPORTANT: we gate this on actually being near the end of the track,
        // not just "IsMusicStreamPlaying() == false" on its own. On Windows,
        // dragging or resizing the window blocks raylib's main loop for the
        // duration of the drag (a well-known WinAPI modal-loop quirk), which
        // can starve the audio stream's buffer and briefly report "not playing"
        // mid-song. Without the near-end check, that got misread as "song
        // ended" and wrongly skipped/stopped playback on every click-drag.
        bool nearEnd = (lengthSec > 0.0f) && (playedSec >= lengthSec - 0.35f);
        if (music.ctxData != nullptr && !pause && !draggingSeek && nearEnd && !IsMusicStreamPlaying(music))
            HandleTrackEnd();

        // Seek: click or drag anywhere on the progress bar to jump playback position.
        if (hoverProgress && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) draggingSeek = true;
        if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) draggingSeek = false;
        if (draggingSeek && music.ctxData != nullptr && lengthSec > 0.0f)
        {
            float ratio = (mouse.x - progressBg.x) / progressBg.width;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            SeekMusicStream(music, ratio * lengthSec);
            playedSec = ratio * lengthSec; // reflect immediately this frame, don't wait a frame
        }

        timePlayed = (lengthSec > 0.0f) ? (playedSec / lengthSec) : 0.0f;
        if (timePlayed > 1.0f) timePlayed = 1.0f;

        minutesTotal  = lengthSec / 60.0f;
        minutesPlayed = playedSec / 60.0f;

        // ================= DRAW =================
        BeginDrawing();
        ClearBackground(COL_BG);

        // ---- Online URL box ----
        {
            Color border = urlBoxActive ? COL_GREEN : COL_TRACK_BG;
            DrawRectangleRounded(urlBox, 0.3f, 8, COL_BAR_BG);
            DrawRectangleRoundedLines(urlBox, 0.3f, 8, border);

            bool showPlaceholder = (urlBufferLen == 0 && !urlBoxActive);
            const char* text = showPlaceholder ? "Paste a direct audio URL and press Enter..." : urlBuffer;
            Color textColor = showPlaceholder ? COL_TEXT_GRAY : COL_TEXT_WHITE;
            DrawTextEx(gFontSmall, text, { urlBox.x + 12, urlBox.y + 7 }, 16, 0, textColor);

            // simple blinking text cursor while focused
            if (urlBoxActive && ((int)(GetTime() * 2) % 2 == 0))
            {
                Vector2 sz = MeasureTextEx(gFontSmall, urlBuffer, 16, 0);
                DrawRectangle((int)(urlBox.x + 12 + sz.x + 2), (int)urlBox.y + 7, 2, 18, COL_TEXT_WHITE);
            }

            if (!urlStatusMsg.empty())
            {
                Color statusColor = urlStatusIsError ? Color{ 230, 80, 80, 255 } : COL_GREEN;
                DrawTextEx(gFontSmall, urlStatusMsg.c_str(), { urlBox.x + 2, urlBox.y + urlBox.height + 6 }, 14, 0, statusColor);
            }
        }

        // ---- Album cover ----
        if (cover.id != 0)
        {
            // soft shadow card behind the cover
            DrawRectangleRounded(
                { coverDest.x - 8, coverDest.y - 8, coverDest.width + 16, coverDest.height + 16 },
                0.04f, 8, COL_BAR_BG);

            BeginScissorMode((int)coverDest.x, (int)coverDest.y, (int)coverDest.width, (int)coverDest.height);
            DrawTexturePro(cover, { 0, 0, (float)cover.width, (float)cover.height },
                           coverDest, { 0, 0 }, 0, WHITE);
            EndScissorMode();
        }
        else
        {
            DrawRectangleRounded(coverDest, 0.04f, 8, COL_BAR_BG);
            const char* hint = "Drop a file";
            Vector2 sz = MeasureTextEx(gFontSmall, hint, 22, 0);
            DrawTextEx(gFontSmall, hint,
                       { coverDest.x + coverDest.width / 2 - sz.x / 2, coverDest.y + coverDest.height / 2 - sz.y / 2 },
                       22, 0, COL_TEXT_GRAY);
        }

        // ---- Title / artist (shaped via HarfBuzz for correct Arabic joining) ----
        if (shaperOk)
        {
            float titleW = gShaperTitle.MeasureText(trackTitle);
            gShaperTitle.DrawText(trackTitle,
                       { sw / 2.0f - titleW / 2, coverDest.y + coverDest.height + 24 },
                       COL_TEXT_WHITE);

            float artistW = gShaperArtist.MeasureText(trackArtist);
            gShaperArtist.DrawText(trackArtist,
                       { sw / 2.0f - artistW / 2, coverDest.y + coverDest.height + 74 },
                       COL_TEXT_GRAY);
        }
        else
        {
            Vector2 titleSz = MeasureTextEx(gFont, trackTitle.c_str(), 34, 0);
            DrawTextEx(gFont, trackTitle.c_str(),
                       { sw / 2.0f - titleSz.x / 2, coverDest.y + coverDest.height + 24 },
                       34, 0, COL_TEXT_WHITE);

            Vector2 artistSz = MeasureTextEx(gFontSmall, trackArtist.c_str(), 22, 0);
            DrawTextEx(gFontSmall, trackArtist.c_str(),
                       { sw / 2.0f - artistSz.x / 2, coverDest.y + coverDest.height + 74 },
                       22, 0, COL_TEXT_GRAY);
        }

        // ---- Playlist position counter ("3 / 10") ----
        if (playlist.size() > 1)
        {
            const char* counter = TextFormat("%d / %d", currentTrackIndex + 1, (int)playlist.size());
            Vector2 counterSz = MeasureTextEx(gFontSmall, counter, 18, 0);
            DrawTextEx(gFontSmall, counter,
                       { sw / 2.0f - counterSz.x / 2, coverDest.y + coverDest.height + 104 },
                       18, 0, COL_TRACK_BG);
        }

        // ---- Bottom player bar ----
        DrawRectangleRec(playerBar, COL_BAR_BG);

        // progress track + fill (rounded, Spotify-style)
        DrawRectangleRounded(progressBg, 1.0f, 6, COL_TRACK_BG);
        Rectangle progressFill = progressBg;
        progressFill.width = progressBg.width * timePlayed;
        if (progressFill.width > 0)
            DrawRectangleRounded(progressFill, 1.0f, 6, COL_GREEN);

        // little scrub dot at current position (only while a track is loaded)
        if (music.ctxData != nullptr)
        {
            Vector2 dot = { progressBg.x + progressFill.width, progressBg.y + progressBg.height / 2 };
            DrawCircleV(dot, 6, COL_TEXT_WHITE);
        }

        // time labels
        DrawTextEx(gFontSmall, TextFormat("%02d:%02d", (int)minutesPlayed, (int)(playedSec - ((int)minutesPlayed) * 60)),
                   { progressBg.x - 55, progressBg.y - 8 }, 20, 0, COL_TEXT_GRAY);
        DrawTextEx(gFontSmall, TextFormat("%02d:%02d", (int)minutesTotal, (int)(lengthSec - ((int)minutesTotal) * 60)),
                   { progressBg.x + progressBg.width + 15, progressBg.y - 8 }, 20, 0, COL_TEXT_GRAY);

        // backward / skip buttons — icons scaled down with padding so they don't
        // touch the edges of their hit circle, and tinted lighter on hover

        DrawIconFit(backwardIcon, backwardBtnCenter, sideBtnRadius * 1.0f,
                    hoverBackward ? COL_TEXT_WHITE : COL_ICON_IDLE);
        DrawIconFit(skipIcon, skipBtnCenter, sideBtnRadius * 1.0f,
                    hoverSkip ? COL_TEXT_WHITE : COL_ICON_IDLE);

        // ---- Shuffle icon: two crossing diagonals with small arrowheads ----
        {
            Color c = shuffleOn ? COL_GREEN : (hoverShuffle ? COL_TEXT_WHITE : COL_ICON_IDLE);
            float x = shuffleBtnCenter.x, y = shuffleBtnCenter.y, r = 7;
            DrawLineEx({ x - r, y - r }, { x + r, y + r }, 2, c);
            DrawLineEx({ x - r, y + r }, { x + r, y - r }, 2, c);
            DrawTriangle({ x + r, y + r }, { x + r - 6, y + r }, { x + r, y + r - 6 }, c);
            DrawTriangle({ x + r, y - r }, { x + r - 6, y - r }, { x + r, y - r + 6 }, c);
        }

        // ---- Repeat icon: circular arrow, small "1" badge when repeat-one ----
        {
            Color c = (repeatMode != RepeatMode::Off) ? COL_GREEN : (hoverRepeat ? COL_TEXT_WHITE : COL_ICON_IDLE);
            float x = repeatBtnCenter.x, y = repeatBtnCenter.y, r = 7;
            DrawRing({ x, y }, r - 2, r, -100, 190, 12, c);
            DrawTriangle({ x + r + 1, y }, { x + r - 5, y - 4 }, { x + r - 5, y + 4 }, c);
            if (repeatMode == RepeatMode::One)
            {
                Vector2 badgeSz = MeasureTextEx(gFontSmall, "1", 14, 0);
                DrawTextEx(gFontSmall, "1", { x - badgeSz.x / 2, y - badgeSz.y / 2 }, 14, 0, c);
            }
        }

        // play/pause button - white circle, hover -> green
        Color playBtnColor = hoverPlay ? COL_GREEN : COL_TEXT_WHITE;
        DrawCircleV(playBtnCenter, playBtnRadius, playBtnColor);
        Texture2D activeIcon = pause ? playIcon : pauseIcon;
        // A play triangle's visual "center of mass" sits left of its bounding-box
        // center (its point trails off to the right), so centering by bounding
        // box alone reads as shifted left. Nudge right by ~12% of the icon size
        // to compensate. The pause icon (two bars) is symmetric, no nudge needed.
        float iconDrawSize = playBtnRadius * 0.8f;
        float opticalOffset = pause ? iconDrawSize * 0.12f : 0.0f;
        DrawIconFit(activeIcon, playBtnCenter, iconDrawSize, BLACK, opticalOffset);

        // ---- Volume control: speaker glyph (drawn, no asset needed) + slider ----
        {
            Color volColor = (hoverSpeaker || hoverVolume || draggingVolume) ? COL_TEXT_WHITE : COL_ICON_IDLE;
            float sx = speakerCenter.x, sy = speakerCenter.y;

            // speaker body (small box) + cone (trapezoid, tip pointing right)
            DrawRectangle((int)(sx - 12), (int)(sy - 4), 5, 8, volColor);
            DrawTriangle({ sx - 7, sy - 4 }, { sx - 7, sy + 4 }, { sx + 2, sy + 10 }, volColor);
            DrawTriangle({ sx - 7, sy - 4 }, { sx + 2, sy + 10 }, { sx + 2, sy - 10 }, volColor);

            if (gVolume <= 0.0f)
            {
                // muted: small X to the right of the cone
                DrawLineEx({ sx + 8, sy - 6 }, { sx + 16, sy + 6 }, 2, volColor);
                DrawLineEx({ sx + 16, sy - 6 }, { sx + 8, sy + 6 }, 2, volColor);
            }
            else
            {
                // sound-wave arcs; second arc only shows above ~40% volume
                DrawRing({ sx + 2, sy }, 6, 9, -40, 40, 8, volColor);
                if (gVolume > 0.4f) DrawRing({ sx + 2, sy }, 11, 14, -35, 35, 8, volColor);
            }

            DrawRectangleRounded(volumeBg, 1.0f, 6, COL_TRACK_BG);
            Rectangle volFill = volumeBg;
            volFill.width = volumeBg.width * gVolume;
            if (volFill.width > 0) DrawRectangleRounded(volFill, 1.0f, 6, COL_TEXT_WHITE);

            Vector2 volDot = { volumeBg.x + volFill.width, volumeBg.y + volumeBg.height / 2 };
            DrawCircleV(volDot, 5, COL_TEXT_WHITE);
        }

        // ---- Command console overlay ----
        if (consoleActive)
        {
            Rectangle consoleBar = { 0, playerBar.y - 40, sw, 36 };
            DrawRectangleRec(consoleBar, Color{ 0, 0, 0, 230 });
            std::string line = ":" + std::string(consoleBuffer);
            DrawTextEx(gFontSmall, line.c_str(), { consoleBar.x + 12, consoleBar.y + 8 }, 18, 0, COL_TEXT_WHITE);
            if (((int)(GetTime() * 2)) % 2 == 0)
            {
                Vector2 sz = MeasureTextEx(gFontSmall, line.c_str(), 18, 0);
                DrawRectangle((int)(consoleBar.x + 12 + sz.x + 2), (int)consoleBar.y + 8, 2, 18, COL_TEXT_WHITE);
            }
        }
        else
        {
            // small persistent hint, bottom-left, so the feature is discoverable
            DrawTextEx(gFontSmall, "` for commands", { 12, playerBar.y - 22 }, 14, 0, COL_TRACK_BG);
        }

        // ---- Tooltip: names whatever control the mouse is currently over ----
        {
            const char* tooltip = nullptr;
            Vector2 anchor = { 0, 0 };

            if      (hoverPlay)     { tooltip = pause ? "Play" : "Pause";                anchor = playBtnCenter; }
            else if (hoverBackward) { tooltip = "Previous / Restart";                    anchor = backwardBtnCenter; }
            else if (hoverSkip)     { tooltip = "Next track";                            anchor = skipBtnCenter; }
            else if (hoverShuffle)  { tooltip = shuffleOn ? "Shuffle: On" : "Shuffle: Off"; anchor = shuffleBtnCenter; }
            else if (hoverRepeat)   { tooltip = repeatMode == RepeatMode::Off ? "Repeat: Off"
                                              : repeatMode == RepeatMode::All ? "Repeat: All" : "Repeat: One";
                                       anchor = repeatBtnCenter; }
            else if (hoverSpeaker)  { tooltip = (gVolume <= 0.0f) ? "Unmute" : "Mute";    anchor = speakerCenter; }
            else if (hoverVolume)   { tooltip = "Volume";  anchor = { volumeBg.x + volumeBg.width / 2, volumeBg.y }; }
            else if (hoverProgress) { tooltip = "Seek";    anchor = { mouse.x, progressBg.y }; }

            if (tooltip != nullptr)
            {
                Vector2 sz = MeasureTextEx(gFontSmall, tooltip, 16, 0);
                float padX = 8, padY = 5;
                Rectangle box = { anchor.x - sz.x / 2 - padX, anchor.y - 46 - sz.y - padY * 2, sz.x + padX * 2, sz.y + padY * 2 };
                if (box.x < 6) box.x = 6;
                if (box.x + box.width > sw - 6) box.x = sw - 6 - box.width;

                DrawRectangleRounded(box, 0.3f, 6, { 0, 0, 0, 220 });
                DrawTextEx(gFontSmall, tooltip, { box.x + padX, box.y + padY }, 16, 0, COL_TEXT_WHITE);
            }
        }

        EndDrawing();

        if (music.ctxData != nullptr) UpdateMusicStream(music);
    }

    UnloadMusicStream(music);
    UnloadTexture(pauseIcon);
    UnloadTexture(playIcon);
    UnloadTexture(skipIcon);
    UnloadTexture(backwardIcon);
    UnloadFont(gFont);
    UnloadFont(gFontSmall);
    gShaperTitle.Shutdown();
    gShaperArtist.Shutdown();

    if (gDownloadThread.joinable()) gDownloadThread.join();
    curl_global_cleanup();

    CloseAudioDevice();
    CloseWindow();
}
