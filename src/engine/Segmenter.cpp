#include "Segmenter.h"

#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <execution>
#include <numeric>

namespace ml = winrt::Windows::AI::MachineLearning;

Segmenter::~Segmenter()
{
    {
        std::lock_guard lk(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

void Segmenter::SetModel(const std::wstring& path)
{
    std::lock_guard lk(m_mutex);
    m_model = path;
}

bool Segmenter::Due(double nowMs)
{
    if (m_unsupported || m_gaveUp) return false;
    std::lock_guard lk(m_mutex);
    return !m_busy && nowMs - m_lastOfferMs >= kIntervalMs;
}

void Segmenter::Reset()
{
    std::lock_guard lk(m_mutex);
    m_reset = true;
    m_hasResult = false;
}

void Segmenter::Offer(const uint8_t* y, const uint8_t* uv, long pitch, uint32_t w, uint32_t h, double nowMs)
{
    if (w < 2 || h < 2) return;
    // The model's input: RGB, (value / 255 - 0.5) / 0.5, planar (NCHW). One sample per cell,
    // BT.601 limited range like the rest of the pipeline.
    std::vector<float> job(3 * kSize * kSize);
    float* r = job.data();
    float* g = r + kSize * kSize;
    float* b = g + kSize * kSize;
    for (uint32_t j = 0; j < kSize; ++j)
    {
        const uint32_t sy = (j * h + h / 2) / kSize;
        const uint8_t* yrow = y + static_cast<size_t>(sy) * pitch;
        const uint8_t* uvrow = uv + static_cast<size_t>(sy / 2) * pitch;
        for (uint32_t i = 0; i < kSize; ++i)
        {
            const uint32_t sx = (i * w + w / 2) / kSize;
            const float yy = 1.164f * (yrow[sx] - 16);
            const float u = uvrow[sx & ~1u] - 128.f, v = uvrow[(sx & ~1u) + 1] - 128.f;
            const size_t at = static_cast<size_t>(j) * kSize + i;
            r[at] = std::clamp(yy + 1.596f * v, 0.f, 255.f) / 127.5f - 1.f;
            g[at] = std::clamp(yy - 0.392f * u - 0.813f * v, 0.f, 255.f) / 127.5f - 1.f;
            b[at] = std::clamp(yy + 2.017f * u, 0.f, 255.f) / 127.5f - 1.f;
        }
    }
    {
        std::lock_guard lk(m_mutex);
        if (m_busy) return;
        m_job.swap(job);
        m_busy = true;
        m_lastOfferMs = nowMs;
        if (!m_thread.joinable()) m_thread = std::thread(&Segmenter::Worker, this);
    }
    m_wake.notify_one();
}

bool Segmenter::TakeMask(std::vector<uint8_t>& mask)
{
    std::lock_guard lk(m_mutex);
    if (!m_hasResult) return false;
    m_hasResult = false;
    mask = m_result;
    return true;
}

void Segmenter::Worker()
{
    bool apartment = false;
    ml::LearningModelSession session{ nullptr };
    winrt::hstring input, output;
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        std::wstring path;
        {
            std::lock_guard lk(m_mutex);
            path = m_model;
        }
        const ml::LearningModel model = ml::LearningModel::LoadFromFilePath(path);
        input = model.InputFeatures().GetAt(0).Name();
        output = model.OutputFeatures().GetAt(0).Name();
        // The GPU when it can run the model, else the CPU (still a few ms at 192 x 192).
        try
        {
            session = ml::LearningModelSession(model, ml::LearningModelDevice(ml::LearningModelDeviceKind::DirectXHighPerformance));
        }
        catch (...)
        {
            session = ml::LearningModelSession(model, ml::LearningModelDevice(ml::LearningModelDeviceKind::Cpu));
        }
    }
    catch (...)
    {
        m_unsupported = true; // no Windows ML (Windows N), the model missing, or an operator it lacks
    }

    int errors = 0;
    std::vector<float> job, out;
    for (;;)
    {
        bool reset = false;
        {
            std::unique_lock lk(m_mutex);
            m_wake.wait(lk, [this] { return m_quit || m_busy; });
            if (m_quit) break;
            job.swap(m_job);
            reset = m_reset;
            m_reset = false;
        }
        bool ok = false;
        std::vector<uint8_t> mask;
        if (!m_unsupported && !m_gaveUp)
        {
            try
            {
                const auto t0 = std::chrono::steady_clock::now();
                ml::LearningModelBinding binding(session);
                binding.Bind(input, ml::TensorFloat::CreateFromArray(std::vector<int64_t>{ 1, 3, kSize, kSize }, job));
                const ml::LearningModelEvaluationResult result = session.Evaluate(binding, L"");
                const ml::TensorFloat tensor = result.Outputs().Lookup(output).as<ml::TensorFloat>();
                const auto view = tensor.GetAsVectorView();
                out.resize(view.Size());
                view.GetMany(0, out);
                m_inferMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

                // Two channels (background, person): probabilities when they add up to 1,
                // else scores (logits).
                const size_t n = static_cast<size_t>(kSize) * kSize;
                std::vector<float> person(n);
                if (out.size() >= 2 * n)
                {
                    const float* c0 = out.data();
                    const float* c1 = out.data() + n;
                    const bool probabilities = std::abs(c0[n / 2] + c1[n / 2] - 1.f) < 0.02f && c1[n / 2] >= 0.f && c1[n / 2] <= 1.f &&
                                               std::abs(c0[0] + c1[0] - 1.f) < 0.02f;
                    for (size_t i = 0; i < n; ++i)
                        person[i] = probabilities ? c1[i] : 1.f / (1.f + std::exp(c0[i] - c1[i]));
                }
                else if (out.size() >= n)
                {
                    for (size_t i = 0; i < n; ++i) person[i] = out[i] >= 0.f && out[i] <= 1.f ? out[i] : 1.f / (1.f + std::exp(-out[i]));
                }
                else
                {
                    throw winrt::hresult_error(E_UNEXPECTED);
                }
                // Over time: half the new look, half the old (no flicker at the edges).
                if (reset || m_smooth.size() != n) m_smooth = person;
                else
                    for (size_t i = 0; i < n; ++i) m_smooth[i] = 0.5f * (m_smooth[i] + person[i]);
                // A firmer edge: 0.3 and below is background, 0.7 and above the person.
                mask.resize(n);
                for (size_t i = 0; i < n; ++i)
                {
                    const float t = std::clamp((m_smooth[i] - 0.3f) / 0.4f, 0.f, 1.f);
                    mask[i] = static_cast<uint8_t>(std::lround(255.f * t * t * (3.f - 2.f * t)));
                }
                ok = true;
                errors = 0;
            }
            catch (...)
            {
                if (++errors >= 10) m_gaveUp = true;
            }
        }
        std::lock_guard lk(m_mutex);
        m_busy = false;
        if (ok)
        {
            m_result.swap(mask);
            m_hasResult = true;
        }
    }
    session = nullptr;
    if (apartment) winrt::uninit_apartment();
}

// ---- blending ----------------------------------------------------------------------------

namespace
{
// Box-averages an NV12 region into tw x th (both even), then softens it twice with a 3 x 3 box.
void Shrink(const uint8_t* y, const uint8_t* uv, long pitch, uint32_t w, uint32_t h, uint32_t tw, uint32_t th,
            std::vector<uint8_t>& tiny, std::vector<uint8_t>& tmp)
{
    tiny.resize(static_cast<size_t>(tw) * th * 3 / 2);
    uint8_t* ty = tiny.data();
    uint8_t* tuv = tiny.data() + static_cast<size_t>(tw) * th;
    for (uint32_t j = 0; j < th; ++j)
        for (uint32_t i = 0; i < tw; ++i)
        {
            const uint32_t x0 = i * w / tw, x1 = std::max(x0 + 1, (i + 1) * w / tw);
            const uint32_t y0 = j * h / th, y1 = std::max(y0 + 1, (j + 1) * h / th);
            uint32_t sum = 0, count = 0;
            for (uint32_t yy = y0; yy < y1; yy += 2) // every other row and column: plenty for a blur
                for (uint32_t xx = x0; xx < x1; xx += 2)
                {
                    sum += y[static_cast<size_t>(yy) * pitch + xx];
                    ++count;
                }
            ty[static_cast<size_t>(j) * tw + i] = static_cast<uint8_t>(sum / std::max(1u, count));
        }
    const uint32_t cw = tw / 2, ch = th / 2, sw = w / 2, sh = h / 2;
    for (uint32_t j = 0; j < ch; ++j)
        for (uint32_t i = 0; i < cw; ++i)
        {
            const uint32_t x0 = i * sw / cw, x1 = std::max(x0 + 1, (i + 1) * sw / cw);
            const uint32_t y0 = j * sh / ch, y1 = std::max(y0 + 1, (j + 1) * sh / ch);
            uint32_t su = 0, sv = 0, count = 0;
            for (uint32_t yy = y0; yy < y1; yy += 2)
                for (uint32_t xx = x0; xx < x1; xx += 2)
                {
                    su += uv[static_cast<size_t>(yy) * pitch + 2 * xx];
                    sv += uv[static_cast<size_t>(yy) * pitch + 2 * xx + 1];
                    ++count;
                }
            count = std::max(1u, count);
            tuv[static_cast<size_t>(j) * tw + 2 * i] = static_cast<uint8_t>(su / count);
            tuv[static_cast<size_t>(j) * tw + 2 * i + 1] = static_cast<uint8_t>(sv / count);
        }
    // Softer: a 3 x 3 box, twice, on each plane (chroma per component).
    auto soften = [&](uint8_t* plane, uint32_t pw, uint32_t ph, uint32_t step) {
        tmp.assign(plane, plane + static_cast<size_t>(pw) * step * ph);
        for (int pass = 0; pass < 2; ++pass)
        {
            for (uint32_t j = 0; j < ph; ++j)
                for (uint32_t i = 0; i < pw; ++i)
                    for (uint32_t c = 0; c < step; ++c)
                    {
                        uint32_t sum = 0, count = 0;
                        for (int dj = -1; dj <= 1; ++dj)
                            for (int di = -1; di <= 1; ++di)
                            {
                                const int jj = static_cast<int>(j) + dj, ii = static_cast<int>(i) + di;
                                if (jj < 0 || ii < 0 || jj >= static_cast<int>(ph) || ii >= static_cast<int>(pw)) continue;
                                sum += tmp[(static_cast<size_t>(jj) * pw + ii) * step + c];
                                ++count;
                            }
                        plane[(static_cast<size_t>(j) * pw + i) * step + c] = static_cast<uint8_t>(sum / count);
                    }
            tmp.assign(plane, plane + static_cast<size_t>(pw) * step * ph);
        }
    };
    soften(ty, tw, th, 1);
    soften(tuv, cw, ch, 2);
}

struct Tap
{
    uint32_t i0, i1;
    uint32_t w1; // 0..256
};

Tap MakeTap(uint32_t x, uint32_t out, uint32_t in)
{
    const double pos = std::max(0.0, (x + 0.5) * in / out - 0.5);
    uint32_t i0 = std::min(static_cast<uint32_t>(pos), in - 1);
    const uint32_t i1 = std::min(i0 + 1, in - 1);
    return { i0, i1, static_cast<uint32_t>((pos - i0) * 256) };
}
} // namespace

void BlendBackground(uint8_t* y, uint8_t* uv, long pitch, uint32_t w, uint32_t h, const std::vector<uint8_t>& mask,
                     bool maskChanged, int strength, const uint8_t* image, BackgroundScratch& s, bool parallel)
{
    constexpr uint32_t K = Segmenter::kSize;
    if (w < 4 || h < 4 || mask.size() != static_cast<size_t>(K) * K) return;

    // The mask at half the picture's size: one value per chroma sample, per 2 x 2 luma.
    const uint32_t aw = w / 2, ah = h / 2;
    if (maskChanged || s.alphaW != aw || s.alphaH != ah)
    {
        s.alphaW = aw;
        s.alphaH = ah;
        s.alpha.resize(static_cast<size_t>(aw) * ah);
        std::vector<Tap> xt(aw);
        for (uint32_t i = 0; i < aw; ++i) xt[i] = MakeTap(i, aw, K);
        for (uint32_t j = 0; j < ah; ++j)
        {
            const Tap t = MakeTap(j, ah, K);
            const uint8_t* r0 = mask.data() + static_cast<size_t>(t.i0) * K;
            const uint8_t* r1 = mask.data() + static_cast<size_t>(t.i1) * K;
            uint8_t* dst = s.alpha.data() + static_cast<size_t>(j) * aw;
            for (uint32_t i = 0; i < aw; ++i)
            {
                const Tap& x = xt[i];
                const uint32_t top = r0[x.i0] * (256 - x.w1) + r0[x.i1] * x.w1;
                const uint32_t bottom = r1[x.i0] * (256 - x.w1) + r1[x.i1] * x.w1;
                dst[i] = static_cast<uint8_t>((top * (256 - t.w1) + bottom * t.w1 + (1u << 15)) >> 16);
            }
        }
    }

    // The blurred picture, small: stronger means bigger blocks (8..40 px at 1080 lines).
    uint32_t tw = 0, th = 0;
    if (!image)
    {
        const double block = (8.0 + std::clamp(strength, 0, 100) * 0.32) * h / 1080.0;
        tw = std::max(4u, static_cast<uint32_t>(w / std::max(2.0, block))) & ~1u;
        th = std::max(4u, static_cast<uint32_t>(h / std::max(2.0, block))) & ~1u;
        Shrink(y, uv, pitch, w, h, tw, th, s.tiny, s.tinyTmp);
    }
    const uint8_t* ty = s.tiny.data();
    const uint8_t* tuv = s.tiny.data() + static_cast<size_t>(tw) * th;
    std::vector<Tap> xl, xc;
    if (!image)
    {
        xl.resize(w);
        for (uint32_t i = 0; i < w; ++i) xl[i] = MakeTap(i, w, tw);
        xc.resize(aw);
        for (uint32_t i = 0; i < aw; ++i) xc[i] = MakeTap(i, aw, tw / 2);
    }

    // Rows in chunks: a luma row pair and its chroma row together.
    std::vector<uint32_t> chunks((ah + 31) / 32);
    std::iota(chunks.begin(), chunks.end(), 0u);
    auto blendChunk = [&](uint32_t chunk) {
        const uint32_t cy0 = chunk * 32, cy1 = std::min(ah, cy0 + 32);
        for (uint32_t cy = cy0; cy < cy1; ++cy)
        {
            const uint8_t* arow = s.alpha.data() + static_cast<size_t>(cy) * aw;
            for (uint32_t k = 0; k < 2; ++k)
            {
                const uint32_t yy = cy * 2 + k;
                uint8_t* row = y + static_cast<size_t>(yy) * pitch;
                if (image)
                {
                    const uint8_t* bg = image + static_cast<size_t>(yy) * w;
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        const uint32_t a = arow[x / 2] + (arow[x / 2] >> 7); // 0..256
                        row[x] = static_cast<uint8_t>((row[x] * a + bg[x] * (256 - a)) >> 8);
                    }
                    continue;
                }
                const Tap t = MakeTap(yy, h, th);
                const uint8_t* r0 = ty + static_cast<size_t>(t.i0) * tw;
                const uint8_t* r1 = ty + static_cast<size_t>(t.i1) * tw;
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t a = arow[x / 2] + (arow[x / 2] >> 7);
                    if (a == 256) continue; // the person: untouched
                    const Tap& xt = xl[x];
                    const uint32_t top = r0[xt.i0] * (256 - xt.w1) + r0[xt.i1] * xt.w1;
                    const uint32_t bottom = r1[xt.i0] * (256 - xt.w1) + r1[xt.i1] * xt.w1;
                    const uint32_t bg = (top * (256 - t.w1) + bottom * t.w1 + (1u << 15)) >> 16;
                    row[x] = static_cast<uint8_t>((row[x] * a + bg * (256 - a)) >> 8);
                }
            }
            uint8_t* crow = uv + static_cast<size_t>(cy) * pitch;
            if (image)
            {
                const uint8_t* bg = image + static_cast<size_t>(w) * h + static_cast<size_t>(cy) * w;
                for (uint32_t x = 0; x < aw; ++x)
                {
                    const uint32_t a = arow[x] + (arow[x] >> 7);
                    crow[2 * x] = static_cast<uint8_t>((crow[2 * x] * a + bg[2 * x] * (256 - a)) >> 8);
                    crow[2 * x + 1] = static_cast<uint8_t>((crow[2 * x + 1] * a + bg[2 * x + 1] * (256 - a)) >> 8);
                }
                continue;
            }
            const Tap t = MakeTap(cy, ah, th / 2);
            const uint8_t* r0 = tuv + static_cast<size_t>(t.i0) * tw;
            const uint8_t* r1 = tuv + static_cast<size_t>(t.i1) * tw;
            for (uint32_t x = 0; x < aw; ++x)
            {
                const uint32_t a = arow[x] + (arow[x] >> 7);
                if (a == 256) continue;
                const Tap& xt = xc[x];
                for (uint32_t c = 0; c < 2; ++c)
                {
                    const uint32_t top = r0[2 * xt.i0 + c] * (256 - xt.w1) + r0[2 * xt.i1 + c] * xt.w1;
                    const uint32_t bottom = r1[2 * xt.i0 + c] * (256 - xt.w1) + r1[2 * xt.i1 + c] * xt.w1;
                    const uint32_t bg = (top * (256 - t.w1) + bottom * t.w1 + (1u << 15)) >> 16;
                    crow[2 * x + c] = static_cast<uint8_t>((crow[2 * x + c] * a + bg * (256 - a)) >> 8);
                }
            }
        }
    };
    if (parallel) std::for_each(std::execution::par, chunks.begin(), chunks.end(), blendChunk);
    else std::for_each(chunks.begin(), chunks.end(), blendChunk);
}
