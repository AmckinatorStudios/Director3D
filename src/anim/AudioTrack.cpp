#include "anim/AudioTrack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "sage/core/Log.h"

namespace d3d {

namespace {

// Чтение little-endian значений из байтового буфера с проверкой границ —
// файл может быть обрезан или вообще не WAV, а падать на чужом файле нельзя.
bool ReadU32(const std::vector<unsigned char>& b, size_t off, unsigned int& out) {
    if (off + 4 > b.size()) return false;
    out = (unsigned int)b[off] | ((unsigned int)b[off + 1] << 8) |
          ((unsigned int)b[off + 2] << 16) | ((unsigned int)b[off + 3] << 24);
    return true;
}
bool ReadU16(const std::vector<unsigned char>& b, size_t off, unsigned short& out) {
    if (off + 2 > b.size()) return false;
    out = (unsigned short)((unsigned int)b[off] | ((unsigned int)b[off + 1] << 8));
    return true;
}
bool TagAt(const std::vector<unsigned char>& b, size_t off, const char* tag) {
    return off + 4 <= b.size() && std::memcmp(&b[off], tag, 4) == 0;
}

} // namespace

bool AudioTrack::DecodeWav(const std::vector<unsigned char>& bytes,
                           std::vector<float>& outMono, int& outSampleRate) {
    outMono.clear();
    outSampleRate = 0;

    // Заголовок RIFF/WAVE: 12 байт, дальше — цепочка чанков {tag, size, data}.
    if (bytes.size() < 44 || !TagAt(bytes, 0, "RIFF") || !TagAt(bytes, 8, "WAVE")) return false;

    unsigned short format = 0, channels = 0, bitsPerSample = 0;
    unsigned int sampleRate = 0;
    size_t dataOffset = 0, dataSize = 0;

    size_t off = 12;
    while (off + 8 <= bytes.size()) {
        unsigned int chunkSize = 0;
        if (!ReadU32(bytes, off + 4, chunkSize)) break;
        const size_t body = off + 8;
        // Обрезанный файл: чанк объявляет больше данных, чем есть — берём остаток.
        const size_t avail = bytes.size() > body ? bytes.size() - body : 0;
        const size_t actual = std::min((size_t)chunkSize, avail);

        if (TagAt(bytes, off, "fmt ") && actual >= 16) {
            ReadU16(bytes, body + 0, format);
            ReadU16(bytes, body + 2, channels);
            ReadU32(bytes, body + 4, sampleRate);
            ReadU16(bytes, body + 14, bitsPerSample);
        } else if (TagAt(bytes, off, "data")) {
            dataOffset = body;
            dataSize = actual;
        }
        // Чанки выровнены по чётной границе (спека RIFF).
        off = body + actual + (actual & 1u);
    }

    if (!dataOffset || !dataSize || !channels || !sampleRate) return false;
    // 1 — PCM, 3 — IEEE float. Всё остальное (ADPCM, сжатие) не разбираем:
    // огибающей не будет, зато и мусора на экране тоже.
    const bool isFloat = (format == 3);
    if (format != 1 && !isFloat) return false;
    const int bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample < 1 || bytesPerSample > 4) return false;
    if (isFloat && bytesPerSample != 4) return false;

    const size_t frameBytes = (size_t)bytesPerSample * channels;
    if (frameBytes == 0) return false;
    const size_t frames = dataSize / frameBytes;
    outMono.resize(frames);
    outSampleRate = (int)sampleRate;

    for (size_t f = 0; f < frames; ++f) {
        float sum = 0.0f;
        for (unsigned short c = 0; c < channels; ++c) {
            const size_t p = dataOffset + f * frameBytes + (size_t)c * bytesPerSample;
            float v = 0.0f;
            if (isFloat) {
                float tmp = 0.0f;
                std::memcpy(&tmp, &bytes[p], sizeof(float));
                v = tmp;
            } else if (bytesPerSample == 1) {
                // 8-битный PCM беззнаковый со смещением 128 (так в спеке).
                v = ((float)bytes[p] - 128.0f) / 128.0f;
            } else if (bytesPerSample == 2) {
                short tmp = (short)((unsigned short)bytes[p] | ((unsigned short)bytes[p + 1] << 8));
                v = (float)tmp / 32768.0f;
            } else if (bytesPerSample == 3) {
                int tmp = (int)bytes[p] | ((int)bytes[p + 1] << 8) | ((int)((signed char)bytes[p + 2]) << 16);
                v = (float)tmp / 8388608.0f;
            } else { // 4 байта, целочисленный
                int tmp = (int)((unsigned int)bytes[p] | ((unsigned int)bytes[p + 1] << 8) |
                                ((unsigned int)bytes[p + 2] << 16) | ((unsigned int)bytes[p + 3] << 24));
                v = (float)tmp / 2147483648.0f;
            }
            sum += v;
        }
        // Волну рисуем по сумме каналов, приведённой к моно: стерео-дорожка
        // выглядит как одна огибающая, а не как две наложенные.
        outMono[f] = sum / (float)channels;
    }
    return true;
}

bool AudioTrack::Load(const std::string& path) {
    Clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        LOG_ERROR("Audio") << "Не удалось открыть звуковой файл: " << path;
        return false;
    }
    m_path = path;

    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
    std::vector<float> mono;
    int rate = 0;
    if (!DecodeWav(bytes, mono, rate) || mono.empty() || rate <= 0) {
        LOG_INFO("Audio") << "Волна не построена (формат не разобран), звук всё равно доступен: " << path;
        return true; // файл есть — движок его проиграет, просто без предпросмотра
    }

    m_length = (float)mono.size() / (float)rate;

    // Децимация в фиксированное число бакетов: пик хранит min/max участка,
    // поэтому короткие громкие удары не пропадают при сжатии, как это было бы
    // при усреднении.
    const int buckets = std::min<int>(kPeakBuckets, (int)mono.size());
    m_peaks.assign((size_t)buckets, Peak{});
    for (int i = 0; i < buckets; ++i) {
        const size_t from = (size_t)((double)i * mono.size() / buckets);
        const size_t to = std::max(from + 1, (size_t)((double)(i + 1) * mono.size() / buckets));
        float lo = mono[from], hi = mono[from];
        for (size_t s = from; s < to && s < mono.size(); ++s) {
            lo = std::min(lo, mono[s]);
            hi = std::max(hi, mono[s]);
        }
        m_peaks[(size_t)i] = Peak{lo, hi};
    }
    LOG_INFO("Audio") << "Звуковая дорожка загружена: " << path << " ("
                      << m_length << " c, " << rate << " Гц)";
    return true;
}

void AudioTrack::Clear() {
    m_path.clear();
    m_length = 0.0f;
    m_peaks.clear();
}

std::vector<AudioTrack::Peak> AudioTrack::Sample(float t0, float t1, int width) const {
    std::vector<Peak> out;
    if (m_peaks.empty() || width <= 0 || m_length <= 0.0f || t1 <= t0) return out;
    out.assign((size_t)width, Peak{});

    for (int x = 0; x < width; ++x) {
        // Время экрана -> время внутри звука (с учётом смещения дорожки).
        const float ta = t0 + (t1 - t0) * ((float)x / (float)width) - Offset;
        const float tb = t0 + (t1 - t0) * ((float)(x + 1) / (float)width) - Offset;
        if (tb <= 0.0f || ta >= m_length) continue;

        const float ua = std::max(ta, 0.0f) / m_length;
        const float ub = std::min(tb, m_length) / m_length;
        size_t ia = (size_t)(ua * (float)m_peaks.size());
        size_t ib = (size_t)(ub * (float)m_peaks.size());
        ia = std::min(ia, m_peaks.size() - 1);
        ib = std::min(std::max(ib, ia + 1), m_peaks.size());

        float lo = m_peaks[ia].Min, hi = m_peaks[ia].Max;
        for (size_t i = ia; i < ib; ++i) {
            lo = std::min(lo, m_peaks[i].Min);
            hi = std::max(hi, m_peaks[i].Max);
        }
        out[(size_t)x] = Peak{lo, hi};
    }
    return out;
}

} // namespace d3d
