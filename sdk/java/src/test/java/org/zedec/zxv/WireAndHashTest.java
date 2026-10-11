// Copyright (c) 2024-2026 Michael Laurence Curzi and 36N9 Genetics, LLC
// SPDX-License-Identifier: Apache-2.0
package org.zedec.zxv;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.zedec.zxv.Vectors.hex;
import static org.zedec.zxv.Vectors.list;
import static org.zedec.zxv.Vectors.map;
import static org.zedec.zxv.Vectors.num;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;
import org.zedec.zxv.crypto.Hashes;
import org.zedec.zxv.internal.Ffi;
import org.zedec.zxv.internal.Native;
import org.zedec.zxv.wire.Ubh168Frame;
import org.zedec.zxv.wire.WireTruth;

/** UBH-168 frames byte for byte (pure Java and C) and the hash vectors. */
class WireAndHashTest {
    @Test
    void ubh168FramesByteForByte() {
        for (Object o : list(Vectors.root().get("ubh168"))) {
            Map<String, Object> v = map(o);
            int tag = Ubh168Frame.tag((int) num(v.get("trunk")), (int) num(v.get("shell")), (int) num(v.get("sync")));
            assertEquals(num(v.get("tag")), tag);
            List<Object> sp = list(v.get("s_plus")), sm = list(v.get("s_minus"));
            int[] p = {(int) num(sp.get(0)), (int) num(sp.get(1)), (int) num(sp.get(2))};
            int[] m = {(int) num(sm.get(0)), (int) num(sm.get(1))};
            Ubh168Frame f = Ubh168Frame.of(tag, p, m);
            byte[] expected = hex(v.get("frame"));
            assertArrayEquals(expected, f.toBytes(), "pure Java pack");
            assertEquals(f, Ubh168Frame.parse(expected), "pure Java unpack");

            // C cross-check: pack, unpack, resolve.
            byte[] cFrame = new byte[21];
            Ffi.check(Ffi.call(Native.wire_pack, (byte) tag, MemorySegment.ofArray(p), MemorySegment.ofArray(m),
                    MemorySegment.ofArray(cFrame)), null);
            assertArrayEquals(expected, cFrame, "libzxv pack");
            byte[] t = new byte[1];
            int[] p2 = new int[3], m2 = new int[2];
            Ffi.check(Ffi.call(Native.wire_unpack, MemorySegment.ofArray(expected), MemorySegment.ofArray(t),
                    MemorySegment.ofArray(p2), MemorySegment.ofArray(m2)), null);
            assertArrayEquals(p, p2);
            assertArrayEquals(m, m2);
            assertEquals(WireTruth.values()[(int) num(v.get("truth"))], f.resolve());
        }
    }

    @Test
    void idlePilotFrames() {
        List<Object> idle = list(Vectors.root().get("ubh168_idle"));
        for (int trunk = 0; trunk < idle.size(); trunk++) {
            byte[] expected = hex(idle.get(trunk));
            assertArrayEquals(expected, Ubh168Frame.idle(trunk).toBytes(), "pure Java idle " + trunk);
            byte[] c = new byte[21];
            Ffi.check(Ffi.call(Native.wire_idle, trunk, MemorySegment.ofArray(c)), null);
            assertArrayEquals(expected, c, "libzxv idle " + trunk);
            assertEquals(WireTruth.NEUTRAL, Ubh168Frame.idle(trunk).resolve());
        }
    }

    @Test
    void zeroCopyFrameIntoNativeMemory() {
        try (Arena a = Arena.ofConfined()) {
            MemorySegment buf = a.allocate(42);
            Ubh168Frame f = Ubh168Frame.idle(3);
            f.writeTo(buf, 21);
            assertEquals(f, Ubh168Frame.parse(buf.asSlice(21)));
        }
    }

    @Test
    void hashes() {
        for (Object o : list(Vectors.root().get("hash"))) {
            Map<String, Object> v = map(o);
            byte[] data = ((String) v.get("utf8")).getBytes(StandardCharsets.UTF_8);
            assertArrayEquals(hex(v.get("keccak256")), Hashes.keccak256(data));
            assertArrayEquals(hex(v.get("sha3_256")), Hashes.sha3_256(data));
            assertArrayEquals(hex(v.get("shake256_64")), Hashes.shake256(data, 64));
            try (Arena a = Arena.ofConfined()) {
                MemorySegment off = a.allocate(Math.max(1, data.length)).asSlice(0, data.length);
                off.copyFrom(MemorySegment.ofArray(data));
                assertArrayEquals(hex(v.get("sha3_256")), Hashes.sha3_256(off), "native segment in place");
            }
        }
        assertNotEquals(Vectors.toHex(Hashes.random(32)), Vectors.toHex(Hashes.random(32)));
    }
}
