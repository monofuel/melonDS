// Copyright 2026 monoMelon contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal melonDS platform layer for the renderer test harness. Threads and semaphores
// are real, since the renderers under test depend on them. Everything else is inert.

#include <condition_variable>
#include <stdarg.h>
#include <stdio.h>
#include <mutex>
#include <string>
#include <thread>

#include "Platform.h"
#include "SPI_Firmware.h"

namespace melonDS::Platform
{

struct Thread
{
    std::thread Handle;
};

struct Semaphore
{
    std::mutex Lock;
    std::condition_variable Changed;
    int Count = 0;
};

struct Mutex
{
    std::mutex Lock;
};

void SignalStop(StopReason, void*) {}

std::string GetLocalFilePath(const std::string& filename) { return filename; }
FileHandle* OpenFile(const std::string&, FileMode) { return nullptr; }
FileHandle* OpenLocalFile(const std::string&, FileMode) { return nullptr; }
bool FileExists(const std::string&) { return false; }
bool LocalFileExists(const std::string&) { return false; }
bool CheckFileWritable(const std::string&) { return false; }
bool CheckLocalFileWritable(const std::string&) { return false; }
bool CloseFile(FileHandle*) { return true; }
bool IsEndOfFile(FileHandle*) { return true; }
bool FileReadLine(char*, int, FileHandle*) { return false; }
u64 FilePosition(FileHandle*) { return 0; }
bool FileSeek(FileHandle*, s64, FileSeekOrigin) { return false; }
void FileRewind(FileHandle*) {}
u64 FileRead(void*, u64, u64, FileHandle*) { return 0; }
bool FileFlush(FileHandle*) { return true; }
u64 FileWrite(const void*, u64, u64, FileHandle*) { return 0; }
u64 FileWriteFormatted(FileHandle*, const char*, ...) { return 0; }
u64 FileLength(FileHandle*) { return 0; }

void Log(LogLevel level, const char* fmt, ...)
{
    if (level < LogLevel::Error)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

Thread* Thread_Create(std::function<void()> func)
{
    return new Thread { std::thread(std::move(func)) };
}

void Thread_Free(Thread* thread)
{
    if (thread->Handle.joinable())
        thread->Handle.join();
    delete thread;
}

void Thread_Wait(Thread* thread)
{
    if (thread->Handle.joinable())
        thread->Handle.join();
}

Semaphore* Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore* sema) { delete sema; }

void Semaphore_Reset(Semaphore* sema)
{
    std::lock_guard<std::mutex> guard(sema->Lock);
    sema->Count = 0;
}

void Semaphore_Wait(Semaphore* sema)
{
    std::unique_lock<std::mutex> guard(sema->Lock);
    sema->Changed.wait(guard, [sema] { return sema->Count > 0; });
    sema->Count--;
}

bool Semaphore_TryWait(Semaphore* sema, int timeout_ms)
{
    std::unique_lock<std::mutex> guard(sema->Lock);
    if (!sema->Changed.wait_for(guard, std::chrono::milliseconds(timeout_ms), [sema] { return sema->Count > 0; }))
        return false;
    sema->Count--;
    return true;
}

void Semaphore_Post(Semaphore* sema, int count)
{
    {
        std::lock_guard<std::mutex> guard(sema->Lock);
        sema->Count += count;
    }
    sema->Changed.notify_all();
}

Mutex* Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex* mutex) { delete mutex; }
void Mutex_Lock(Mutex* mutex) { mutex->Lock.lock(); }
void Mutex_Unlock(Mutex* mutex) { mutex->Lock.unlock(); }
bool Mutex_TryLock(Mutex* mutex) { return mutex->Lock.try_lock(); }

void Sleep(u64) {}
u64 GetMSCount() { return 0; }
u64 GetUSCount() { return 0; }

void WriteNDSSave(const u8*, u32, u32, u32, void*) {}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
void WriteFirmware(const Firmware&, u32, u32, void*) {}
void WriteDateTime(int, int, int, int, int, int, void*) {}

void MP_Begin(void*) {}
void MP_End(void*) {}
int MP_SendPacket(u8*, int, u64, void*) { return 0; }
int MP_RecvPacket(u8*, u64*, void*) { return 0; }
int MP_SendCmd(u8*, int, u64, void*) { return 0; }
int MP_SendReply(u8*, int, u64, u16, void*) { return 0; }
int MP_SendAck(u8*, int, u64, void*) { return 0; }
int MP_RecvHostPacket(u8*, u64*, void*) { return 0; }
u16 MP_RecvReplies(u8*, u64, u16, void*) { return 0; }

int Net_SendPacket(u8*, int, void*) { return 0; }
int Net_RecvPacket(u8*, void*) { return 0; }

void Camera_Start(int, void*) {}
void Camera_Stop(int, void*) {}
void Camera_CaptureFrame(int, u32*, int, int, bool, void*) {}

void Mic_Start(void*) {}
void Mic_Stop(void*) {}
int Mic_ReadInput(s16*, int, void*) { return 0; }

AACDecoder* AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder*) {}
bool AAC_Configure(AACDecoder*, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder*, const void*, int, void*, int) { return false; }

bool Addon_KeyDown(KeyType, void*) { return false; }
void Addon_RumbleStart(u32, void*) {}
void Addon_RumbleStop(void*) {}
float Addon_MotionQuery(MotionQueryType, void*) { return 0.0f; }

DynamicLibrary* DynamicLibrary_Load(const char*) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary*) {}
void* DynamicLibrary_LoadFunction(DynamicLibrary*, const char*) { return nullptr; }

}
