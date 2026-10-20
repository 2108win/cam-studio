using System;
using System.Collections.Generic;
using System.Text;

namespace CamStudio.Services;

// A small QR code encoder (ISO/IEC 18004): byte mode, error correction level M, versions 1–10
// (up to 213 bytes), the mask with the lowest penalty. Enough for a download link; no library.
public static class QrCode
{
    // EC codewords per block, then the blocks' data codeword counts, for level M, versions 1..10.
    private static readonly (int Ec, int[] Blocks)[] LevelM =
    {
        (10, new[] { 16 }),
        (16, new[] { 28 }),
        (26, new[] { 44 }),
        (18, new[] { 32, 32 }),
        (24, new[] { 43, 43 }),
        (16, new[] { 27, 27, 27, 27 }),
        (18, new[] { 31, 31, 31, 31 }),
        (22, new[] { 38, 38, 39, 39 }),
        (22, new[] { 36, 36, 36, 37, 37 }),
        (26, new[] { 43, 43, 43, 43, 44 }),
    };

    private static readonly int[][] Alignment =
    {
        Array.Empty<int>(), new[] { 6, 18 }, new[] { 6, 22 }, new[] { 6, 26 }, new[] { 6, 30 },
        new[] { 6, 34 }, new[] { 6, 22, 38 }, new[] { 6, 24, 42 }, new[] { 6, 26, 46 }, new[] { 6, 28, 50 },
    };

    // true = dark. Size 17 + 4 * version, no quiet zone (draw 4 modules of white around it).
    public static bool[,] Encode(string text)
    {
        byte[] data = Encoding.UTF8.GetBytes(text);
        int version = 0;
        for (int v = 1; v <= 10 && version == 0; v++)
        {
            int capacity = Sum(LevelM[v - 1].Blocks) * 8;
            int needed = 4 + (v < 10 ? 8 : 16) + data.Length * 8;
            if (needed <= capacity) version = v;
        }
        if (version == 0) throw new ArgumentException("Too long for a QR code of version 10", nameof(text));
        byte[] codewords = Interleave(version, DataCodewords(version, data));
        return Draw(version, codewords);
    }

    // Mode, count, the bytes, terminator, padding (ISO 18004 7.4).
    internal static byte[] DataCodewords(int version, byte[] data)
    {
        int total = Sum(LevelM[version - 1].Blocks);
        var bits = new List<bool>();
        void Put(int value, int count)
        {
            for (int i = count - 1; i >= 0; i--) bits.Add(((value >> i) & 1) != 0);
        }
        Put(0b0100, 4);
        Put(data.Length, version < 10 ? 8 : 16);
        foreach (byte b in data) Put(b, 8);
        Put(0, Math.Min(4, total * 8 - bits.Count));
        while (bits.Count % 8 != 0) bits.Add(false);
        var bytes = new List<byte>();
        for (int i = 0; i < bits.Count; i += 8)
        {
            int b = 0;
            for (int j = 0; j < 8; j++) b = (b << 1) | (bits[i + j] ? 1 : 0);
            bytes.Add((byte)b);
        }
        for (int pad = 0; bytes.Count < total; pad++) bytes.Add(pad % 2 == 0 ? (byte)0xEC : (byte)0x11);
        return bytes.ToArray();
    }

    // Splits into blocks, adds each block's Reed–Solomon codewords, interleaves (7.6).
    private static byte[] Interleave(int version, byte[] data)
    {
        var (ec, sizes) = LevelM[version - 1];
        var blocks = new List<byte[]>();
        var ecs = new List<byte[]>();
        int at = 0;
        foreach (int size in sizes)
        {
            byte[] block = data[at..(at + size)];
            at += size;
            blocks.Add(block);
            ecs.Add(ReedSolomon(block, ec));
        }
        var result = new List<byte>();
        for (int i = 0; i < sizes[^1]; i++)
            foreach (byte[] b in blocks)
                if (i < b.Length) result.Add(b[i]);
        for (int i = 0; i < ec; i++)
            foreach (byte[] e in ecs) result.Add(e[i]);
        return result.ToArray();
    }

    // GF(256) with x^8 + x^4 + x^3 + x^2 + 1; the generator has roots a^0 .. a^(n-1).
    internal static byte[] ReedSolomon(byte[] data, int count)
    {
        int[] exp = new int[512], log = new int[256];
        for (int i = 0, x = 1; i < 255; i++)
        {
            exp[i] = x;
            log[x] = i;
            x <<= 1;
            if (x >= 256) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        int Mul(int a, int b) => a == 0 || b == 0 ? 0 : exp[log[a] + log[b]];

        int[] gen = { 1 };
        for (int i = 0; i < count; i++)
        {
            var next = new int[gen.Length + 1];
            for (int j = 0; j < gen.Length; j++)
            {
                next[j] ^= gen[j];
                next[j + 1] ^= Mul(gen[j], exp[i]);
            }
            gen = next;
        }
        var rem = new int[count];
        foreach (byte d in data)
        {
            int factor = d ^ rem[0];
            Array.Copy(rem, 1, rem, 0, count - 1);
            rem[count - 1] = 0;
            for (int j = 0; j < count; j++) rem[j] ^= Mul(gen[j + 1], factor);
        }
        var output = new byte[count];
        for (int i = 0; i < count; i++) output[i] = (byte)rem[i];
        return output;
    }

    // 15-bit format information for level M (bits 00) and a mask, BCH(15,5), XOR 0x5412.
    internal static int FormatBits(int mask)
    {
        int value = (0b00 << 3) | mask;
        int rem = value;
        for (int i = 0; i < 10; i++) rem = (rem << 1) ^ (((rem >> 9) & 1) * 0x537);
        return ((value << 10) | (rem & 0x3FF)) ^ 0x5412;
    }

    private static int VersionBits(int version)
    {
        int rem = version;
        for (int i = 0; i < 12; i++) rem = (rem << 1) ^ (((rem >> 11) & 1) * 0x1F25);
        return (version << 12) | (rem & 0xFFF);
    }

    private static bool[,] Draw(int version, byte[] codewords)
    {
        int n = 17 + 4 * version;
        var dark = new bool[n, n];     // [row, column]
        var fixedAt = new bool[n, n];  // function patterns: never masked, never data

        void Set(int r, int c, bool d)
        {
            dark[r, c] = d;
            fixedAt[r, c] = true;
        }
        void Finder(int r0, int c0)
        {
            for (int r = -1; r <= 7; r++)
                for (int c = -1; c <= 7; c++)
                {
                    int rr = r0 + r, cc = c0 + c;
                    if (rr < 0 || cc < 0 || rr >= n || cc >= n) continue;
                    bool ring = r is 0 or 6 || c is 0 or 6;
                    bool core = r is >= 2 and <= 4 && c is >= 2 and <= 4;
                    Set(rr, cc, r is >= 0 and <= 6 && c is >= 0 and <= 6 && (ring || core));
                }
        }
        Finder(0, 0);
        Finder(0, n - 7);
        Finder(n - 7, 0);
        for (int i = 8; i < n - 8; i++)
        {
            Set(6, i, i % 2 == 0);
            Set(i, 6, i % 2 == 0);
        }
        int[] align = Alignment[version - 1];
        int last = align.Length - 1;
        for (int ai = 0; ai < align.Length; ai++)
            for (int aj = 0; aj < align.Length; aj++)
            {
                if (ai == 0 && aj == 0 || ai == 0 && aj == last || ai == last && aj == 0) continue; // a finder's corner
                int r = align[ai], c = align[aj];
                for (int dr = -2; dr <= 2; dr++)
                    for (int dc = -2; dc <= 2; dc++)
                        Set(r + dr, c + dc, Math.Max(Math.Abs(dr), Math.Abs(dc)) != 1);
            }
        Set(n - 8, 8, true); // the dark module
        // Reserve the format areas (written after masking) and the version areas (v7+).
        for (int i = 0; i < 9; i++)
        {
            if (!fixedAt[8, i]) Set(8, i, false);
            if (!fixedAt[i, 8]) Set(i, 8, false);
        }
        for (int i = 0; i < 8; i++)
        {
            Set(8, n - 1 - i, false);
            if (i < 7) Set(n - 1 - i, 8, false);
        }
        if (version >= 7)
        {
            int bits = VersionBits(version);
            for (int i = 0; i < 18; i++)
            {
                bool d = ((bits >> i) & 1) != 0;
                Set(n - 11 + i % 3, i / 3, d);
                Set(i / 3, n - 11 + i % 3, d);
            }
        }

        // Data, in two-module columns from the bottom right, up and down in turn (7.7.3).
        int bit = 0, total = codewords.Length * 8;
        for (int right = n - 1, upward = 1; right > 0; right -= 2, upward ^= 1)
        {
            if (right == 6) right = 5; // the vertical timing column
            for (int k = 0; k < n; k++)
            {
                int r = upward == 1 ? n - 1 - k : k;
                for (int c = right; c >= right - 1; c--)
                {
                    if (fixedAt[r, c]) continue;
                    dark[r, c] = bit < total && ((codewords[bit >> 3] >> (7 - (bit & 7))) & 1) != 0;
                    bit++;
                }
            }
        }

        // The mask with the lowest penalty (8.8.2).
        bool[,]? best = null;
        int bestPenalty = int.MaxValue;
        for (int mask = 0; mask < 8; mask++)
        {
            var m = (bool[,])dark.Clone();
            for (int r = 0; r < n; r++)
                for (int c = 0; c < n; c++)
                    if (!fixedAt[r, c] && Masked(mask, r, c)) m[r, c] = !m[r, c];
            WriteFormat(m, n, FormatBits(mask));
            int p = Penalty(m, n);
            if (p < bestPenalty)
            {
                bestPenalty = p;
                best = m;
            }
        }
        return best!;
    }

    private static bool Masked(int mask, int r, int c) => mask switch
    {
        0 => (r + c) % 2 == 0,
        1 => r % 2 == 0,
        2 => c % 3 == 0,
        3 => (r + c) % 3 == 0,
        4 => (r / 2 + c / 3) % 2 == 0,
        5 => r * c % 2 + r * c % 3 == 0,
        6 => (r * c % 2 + r * c % 3) % 2 == 0,
        _ => ((r + c) % 2 + r * c % 3) % 2 == 0,
    };

    // Bit 14 is the most significant (7.9.1).
    private static void WriteFormat(bool[,] m, int n, int bits)
    {
        bool B(int i) => ((bits >> i) & 1) != 0;
        // Around the top-left finder.
        for (int i = 0; i <= 5; i++) m[8, i] = B(14 - i);
        m[8, 7] = B(8);
        m[8, 8] = B(7);
        m[7, 8] = B(6);
        for (int i = 9; i <= 14; i++) m[14 - i, 8] = B(14 - i);
        // Split between the other two finders.
        for (int i = 0; i <= 6; i++) m[n - 1 - i, 8] = B(14 - i);
        for (int i = 7; i <= 14; i++) m[8, n - 15 + i] = B(14 - i);
        m[n - 8, 8] = true;
    }

    private static int Penalty(bool[,] m, int n)
    {
        int p = 0;
        // N1: runs of five or more of one colour, in rows and columns.
        for (int pass = 0; pass < 2; pass++)
            for (int a = 0; a < n; a++)
            {
                int run = 1;
                for (int b = 1; b < n; b++)
                {
                    bool same = pass == 0 ? m[a, b] == m[a, b - 1] : m[b, a] == m[b - 1, a];
                    if (same) run++;
                    else
                    {
                        if (run >= 5) p += run - 2;
                        run = 1;
                    }
                }
                if (run >= 5) p += run - 2;
            }
        // N2: 2x2 blocks of one colour.
        for (int r = 0; r < n - 1; r++)
            for (int c = 0; c < n - 1; c++)
                if (m[r, c] == m[r, c + 1] && m[r, c] == m[r + 1, c] && m[r, c] == m[r + 1, c + 1]) p += 3;
        // N3: 1:1:3:1:1 finder-like patterns with four light modules on one side.
        bool[] pattern = { true, false, true, true, true, false, true, false, false, false, false };
        for (int pass = 0; pass < 2; pass++)
            for (int a = 0; a < n; a++)
                for (int b = 0; b + 11 <= n; b++)
                {
                    bool fwd = true, back = true;
                    for (int k = 0; k < 11; k++)
                    {
                        bool v = pass == 0 ? m[a, b + k] : m[b + k, a];
                        if (v != pattern[k]) fwd = false;
                        if (v != pattern[10 - k]) back = false;
                    }
                    if (fwd) p += 40;
                    if (back) p += 40;
                }
        // N4: how far the dark share is from half.
        int darkCount = 0;
        foreach (bool v in m) if (v) darkCount++;
        int percent = darkCount * 100 / (n * n);
        p += Math.Abs(percent - 50) / 5 * 10;
        return p;
    }

    private static int Sum(int[] values)
    {
        int s = 0;
        foreach (int v in values) s += v;
        return s;
    }
}
