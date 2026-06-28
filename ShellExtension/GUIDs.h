#pragma once
#include <windows.h>

// Генерируй свои через Tools → Create GUID в VS (IMPLEMENT_OLECREATE формат)
// Или используй эти (они уникальны для твоего проекта)

// CLSID для нашего ShellExtension объекта
// {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
static const CLSID CLSID_FFmpegShellExt =
{ 0xa1b2c3d4, 0xe5f6, 0x7890,
  { 0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90 } };