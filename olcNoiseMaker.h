/*
    OneLoneCoder.com - Simple Audio Noisy Thing
    "Allows you to simply listen to that waveform!" - @Javidx9

    macOS port: replaces WinMM backend with CoreAudio AudioQueue.
    Public API is identical to the original; no changes to main*.cpp needed.

    Original license: GNU GPLv3
    https://github.com/OneLoneCoder/videos/blob/master/LICENSE
*/

#pragma once

#include <iostream>
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <atomic>
#include <type_traits>
using namespace std;

#include <AudioToolbox/AudioToolbox.h>
#include "platform_mac.h"

#ifndef FTYPE
#define FTYPE double
#endif

const double PI = 2.0 * acos(0.0);

template<class T>
class olcNoiseMaker
{
public:
    olcNoiseMaker(wstring sOutputDevice,
                  unsigned int nSampleRate   = 44100,
                  unsigned int nChannels     = 1,
                  unsigned int nBlocks       = 8,
                  unsigned int nBlockSamples = 512)
    {
        Create(sOutputDevice, nSampleRate, nChannels, nBlocks, nBlockSamples);
    }

    ~olcNoiseMaker()
    {
        Stop();
        if (m_audioQueue) {
            AudioQueueDispose(m_audioQueue, true);
            m_audioQueue = nullptr;
        }
    }

    bool Create(wstring sOutputDevice,
                unsigned int nSampleRate   = 44100,
                unsigned int nChannels     = 1,
                unsigned int nBlocks       = 8,
                unsigned int nBlockSamples = 512)
    {
        m_nSampleRate    = nSampleRate;
        m_nChannels      = nChannels;
        m_nBlockCount    = nBlocks;
        m_nBlockSamples  = nBlockSamples;
        m_userFunction   = nullptr;
        m_userFunction_old = nullptr;
        m_dGlobalTime    = 0.0;
        m_bReady         = false;

        // Always use 32-bit float PCM — macOS CoreAudio prefers float and
        // reliably converts it to whatever the hardware actually needs.
        // (T still drives the user-callback math; it no longer controls the
        //  audio-buffer layout.)
        AudioStreamBasicDescription fmt = {};
        fmt.mSampleRate       = nSampleRate;
        fmt.mFormatID         = kAudioFormatLinearPCM;
        fmt.mFormatFlags      = kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked;
        fmt.mChannelsPerFrame = nChannels;
        fmt.mFramesPerPacket  = 1;
        fmt.mBitsPerChannel   = 32;
        fmt.mBytesPerFrame    = (UInt32)(sizeof(float) * nChannels);
        fmt.mBytesPerPacket   = (UInt32)(sizeof(float) * nChannels);

        OSStatus st = AudioQueueNewOutput(&fmt, audioQueueCallback, this,
                                          nullptr, nullptr, 0, &m_audioQueue);
        if (st != noErr) {
            fprintf(stderr, "[olcNoiseMaker] AudioQueueNewOutput failed: %d\n", (int)st);
            return false;
        }

        // Ensure output volume is at maximum.
        AudioQueueSetParameter(m_audioQueue, kAudioQueueParam_Volume, 1.0f);

        // Allocate and pre-fill buffers with silence.
        UInt32 bufBytes = (UInt32)(nBlockSamples * sizeof(float) * nChannels);
        for (unsigned int i = 0; i < nBlocks; i++) {
            AudioQueueBufferRef buf;
            AudioQueueAllocateBuffer(m_audioQueue, bufBytes, &buf);
            buf->mAudioDataByteSize = bufBytes;
            memset(buf->mAudioData, 0, bufBytes);
            AudioQueueEnqueueBuffer(m_audioQueue, buf, 0, nullptr);
        }

        OSStatus startSt = AudioQueueStart(m_audioQueue, nullptr);
        if (startSt != noErr) {
            fprintf(stderr, "[olcNoiseMaker] AudioQueueStart failed: %d\n", (int)startSt);
            return false;
        }

        startKeyboard();
        return true;
    }

    bool Destroy() { return false; }

    void Stop()
    {
        m_bReady = false;
        if (m_audioQueue)
            AudioQueueStop(m_audioQueue, true);
    }

    // Override to generate samples (subclassing path, rarely used).
    virtual double UserProcess(double dTime)           { return 0.0; }
    virtual FTYPE  UserProcess(int nChannel, FTYPE dTime) { return 0.0; }

    FTYPE GetTime() { return m_dGlobalTime; }

    // Returns a list of available output devices (always "Default" on Mac).
    static vector<wstring> Enumerate()
    {
        return { L"Default" };
    }

    // For main1/2: callback signature double(double).
    void SetUserFunction(double(*func)(double))
    {
        m_userFunction_old.store(func,    std::memory_order_relaxed);
        m_userFunction.store(nullptr,     std::memory_order_relaxed);
        m_bReady.store(true, std::memory_order_release);
    }

    // For main3a/4: callback signature FTYPE(int channel, FTYPE time).
    void SetUserFunction(FTYPE(*func)(int, FTYPE))
    {
        m_userFunction.store(func,        std::memory_order_relaxed);
        m_userFunction_old.store(nullptr, std::memory_order_relaxed);
        m_bReady.store(true, std::memory_order_release);
    }

    FTYPE clip(FTYPE dSample, FTYPE dMax)
    {
        if (dSample >= 0.0) return fmin(dSample, dMax);
        else                return fmax(dSample, -dMax);
    }

private:
    static void audioQueueCallback(void* userData, AudioQueueRef queue,
                                   AudioQueueBufferRef buffer)
    {
        static_cast<olcNoiseMaker*>(userData)->fillBuffer(queue, buffer);
    }

    void fillBuffer(AudioQueueRef queue, AudioQueueBufferRef buffer)
    {
        float*       data     = static_cast<float*>(buffer->mAudioData);
        unsigned int nSamples = (unsigned int)(buffer->mAudioDataBytesCapacity
                                               / (sizeof(float) * m_nChannels));

        bool ready = m_bReady.load(std::memory_order_acquire);

        if (!ready) {
            memset(buffer->mAudioData, 0, buffer->mAudioDataBytesCapacity);
            buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
            AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
            return;
        }

        auto fnNew = m_userFunction.load(std::memory_order_relaxed);
        auto fnOld = m_userFunction_old.load(std::memory_order_relaxed);

        for (unsigned int n = 0; n < nSamples; n++) {
            for (unsigned int c = 0; c < m_nChannels; c++) {
                FTYPE sample;
                if (fnNew)
                    sample = clip(fnNew(c, m_dGlobalTime), 1.0);
                else if (fnOld)
                    sample = clip((FTYPE)fnOld((double)m_dGlobalTime), 1.0);
                else
                    sample = clip(UserProcess(c, m_dGlobalTime), 1.0);

                data[n * m_nChannels + c] = (float)sample;
            }
            m_dGlobalTime = m_dGlobalTime + (FTYPE)1.0 / (FTYPE)m_nSampleRate;
        }

        buffer->mAudioDataByteSize = buffer->mAudioDataBytesCapacity;
        AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    }

    using FnNew = FTYPE(*)(int, FTYPE);
    using FnOld = double(*)(double);
    std::atomic<FnNew> m_userFunction{nullptr};
    std::atomic<FnOld> m_userFunction_old{nullptr};

    unsigned int m_nSampleRate;
    unsigned int m_nChannels;
    unsigned int m_nBlockCount;
    unsigned int m_nBlockSamples;

    atomic<bool>  m_bReady;
    atomic<FTYPE> m_dGlobalTime;
    AudioQueueRef m_audioQueue = nullptr;
};
