#include <raylib.h>
#include <iostream>
Texture2D LoadCover(const char* mp3Path)
{
    // حذف الصورة القديمة إن وجدت
    remove("cover.png");

    // استدعاء ffmpeg لاستخراج الكفر
    std::string cmd =
        "ffmpeg -y -i \"" +
        std::string(mp3Path) +
        "\" -an -c:v png cover.png >nul 2>nul";

    int ret = system(cmd.c_str());

    if (ret != 0)
    {
        std::cout << "FFmpeg failed\n";
        return {0};
    }

    if (!FileExists("cover.png"))
    {
        std::cout << "No cover found\n";
        return {0};
    }

    Texture2D tex = LoadTexture("cover.png");

    if (tex.id == 0)
    {
        std::cout << "Failed loading cover.png\n";
        return {0};
    }

    std::cout << "Cover loaded: "
              << tex.width << "x"
              << tex.height << std::endl;

    return tex;
}