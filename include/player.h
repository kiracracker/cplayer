#pragma once

#include <raylib.h>

void InitPlayer();

void UpdatePlayer();

void ClosePlayer();

void LoadSong(const char* path);

extern Music music;
extern Texture2D cover;