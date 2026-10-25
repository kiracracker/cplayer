#include <raylib.h>
#include <stdlib.h> 

Texture2D LoadImageTexture(const char *fileName)
{
    Image image = LoadImage(fileName);

    if (image.data == NULL)
    {
        TraceLog(LOG_ERROR, "Failed to load image: %s", fileName);
        Texture2D empty = {0};
        return empty;
    }

    Texture2D texture = LoadTextureFromImage(image);

    UnloadImage(image);

    return texture;
}