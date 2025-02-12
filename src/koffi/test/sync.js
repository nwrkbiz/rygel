#!/usr/bin/env node

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

const koffi = require('./build/koffi.node');
const assert = require('assert');

const Pack1 = koffi.struct('Pack1', {
    a: 'int'
});
const Pack2 = koffi.struct('Pack2', {
    a: 'int',
    b: 'int'
});
const Pack3 = koffi.struct('Pack3', {
    a: 'int',
    b: 'int',
    c: 'int'
});

const Float2 = koffi.struct('Float2', {
    a: 'float',
    b: 'float'
});
const Float3 = koffi.struct('Float3', {
    a: 'float',
    b: koffi.array('float', 2)
});

const Double2 = koffi.struct('Double2', {
    a: 'double',
    b: 'double'
});
const Double3 = koffi.struct('Double3', {
    a: 'double',
    s: koffi.struct({
        b: 'double',
        c: 'double'
    })
});

const FloatInt = koffi.struct('FloatInt', {
    f: 'float',
    i: 'int'
});
const IntFloat = koffi.struct('IntFloat', {
    i: 'int',
    f: 'float'
});

const BFG = koffi.struct('BFG', {
    a: 'int8_t',
    e: [8, 'short'],
    b: 'int64_t',
    c: 'char',
    d: 'const char *',
    inner: koffi.struct({
        f: 'float',
        g: 'double'
    })
});
const PackedBFG = koffi.pack('PackedBFG', {
    a: 'int8_t',
    b: 'int64_t',
    c: 'char',
    d: 'char *',
    e: 'short',
    inner: koffi.pack({
        f: 'float',
        g: 'double'
    })
});
const AliasBFG = koffi.alias('AliasBFG', PackedBFG);

const FixedString = koffi.struct('FixedString', {
    buf: koffi.array('int8', 64)
});
const FixedString2 = koffi.struct('FixedString2', {
    buf: koffi.array('char', 64)
});

const FixedWide = koffi.struct('FixedWide', {
    buf: koffi.array('char16', 64, 'typed')
});
const FixedWide2 = koffi.struct('FixedWide2', {
    buf: koffi.array('char16', 64)
});

const SingleU32 = koffi.struct('SingleU32', { v: 'uint32_t' });
const SingleU64 = koffi.struct('SingleU64', { v: 'uint64_t' });
const SingleI64 = koffi.struct('SingleI64', { v: 'int64_t' });

const IntContainer = koffi.struct('IntContainer', {
    values: koffi.array('int', 16),
    len: 'int'
});

const StrFree = koffi.disposable('str_free', koffi.types.str, koffi.free);
const Str16Free = koffi.disposable('str16_free', 'str16');

const StrStruct = koffi.struct('StrStruct', {
    str: 'str',
    str16: koffi.types.string16
});

const EndianInts = koffi.struct('EndianInts', {
    i16le: 'int16_le_t',
    i16be: 'int16_be_t',
    u16le: 'uint16_le_t',
    u16be: 'uint16_be_t',
    i32le: 'int32_le_t',
    i32be: 'int32_be_t',
    u32le: 'uint32_le_t',
    u32be: 'uint32_be_t',
    i64le: 'int64_le_t',
    i64be: 'int64_be_t',
    u64le: 'uint64_le_t',
    u64be: 'uint64_be_t'
});

const BigText = koffi.struct('BigText', {
    text: koffi.array('uint8_t', 262145)
});

main();

async function main() {
    try {
        await test();
        console.log('Success!');
    } catch (err) {
        console.error(err);
        process.exit(1);
    }
}

async function test() {
    let lib_filename = __dirname + '/build/misc' + koffi.extension;
    let lib = koffi.load(lib_filename);

    const GetMinusOne1 = lib.func('int8_t GetMinusOne1(void)');
    const GetMinusOne2 = lib.func('int16_t GetMinusOne2(void)');
    const GetMinusOne4 = lib.func('int32_t GetMinusOne4(void)');
    const GetMinusOne8 = lib.func('int64_t GetMinusOne8(void *dummy)');
    const FillPack1 = lib.func('FillPack1', 'void', ['int', koffi.out(koffi.pointer(Pack1))]);
    const RetPack1 = lib.func('RetPack1', Pack1, ['int']);
    const AddPack1 = lib.fastcall('AddPack1', 'void', ['int', koffi.inout(koffi.pointer(Pack1))]);
    const FillPack2 = lib.func('FillPack2', 'void', ['int', 'int', koffi.out(koffi.pointer(Pack2))]);
    const RetPack2 = lib.func('RetPack2', Pack2, ['int', 'int']);
    const AddPack2 = lib.fastcall('AddPack2', 'void', ['int', 'int', koffi.inout(koffi.pointer(Pack2))]);
    const FillPack3 = lib.func('FillPack3', 'void', ['int', 'int', 'int', koffi.out(koffi.pointer(Pack3))]);
    const RetPack3 = lib.func('RetPack3', Pack3, ['int', 'int', 'int']);
    const AddPack3 = lib.fastcall('AddPack3', 'void', ['int', 'int', 'int', koffi.inout(koffi.pointer(Pack3))]);
    const PackFloat2 = lib.func('Float2 PackFloat2(float a, float b, _Out_ Float2 *out)');
    const ThroughFloat2 = lib.func('Float2 ThroughFloat2(Float2 f2)');
    const PackFloat3 = lib.func('Float3 PackFloat3(float a, float b, float c, _Out_ Float3 *out)');
    const ThroughFloat3 = lib.func('Float3 ThroughFloat3(Float3 f3)');
    const PackDouble2 = lib.func('Double2 PackDouble2(double a, double b, _Out_ Double2 *out)');
    const PackDouble3 = lib.func('Double3 PackDouble3(double a, double b, double c, _Out_ Double3 *out)');
    const ReverseFloatInt = lib.func('IntFloat ReverseFloatInt(FloatInt sfi)');
    const ReverseIntFloat = lib.func('FloatInt ReverseIntFloat(IntFloat sif)');
    const ConcatenateToInt1 = lib.func('ConcatenateToInt1', 'int64_t', Array(12).fill('int8_t'));
    const ConcatenateToInt4 = lib.func('ConcatenateToInt4', 'int64_t', Array(12).fill('int32_t'));
    const ConcatenateToInt8 = lib.func('ConcatenateToInt8', 'int64_t', Array(12).fill('int64_t'));
    const ConcatenateToStr1 = lib.func('ConcatenateToStr1', 'str', [...Array(8).fill('int8_t'), koffi.struct('IJK1', {i: 'int8_t', j: 'int8_t', k: 'int8_t'}), 'int8_t']);
    const ConcatenateToStr4 = lib.func('ConcatenateToStr4', 'str', [...Array(8).fill('int32_t'), koffi.pointer(koffi.struct('IJK4', {i: 'int32_t', j: 'int32_t', k: 'int32_t'})), 'int32_t']);
    const ConcatenateToStr8 = lib.func('ConcatenateToStr8', 'str', [...Array(8).fill('int64_t'), koffi.struct('IJK8', {i: 'int64_t', j: 'int64_t', k: 'int64_t'}), 'int64_t']);
    const MakeBFG = lib.func('BFG __stdcall MakeBFG(_Out_ BFG *p, int x, double y, const char *str)');
    const MakePackedBFG = lib.func('AliasBFG __fastcall MakePackedBFG(int x, double y, _Out_ PackedBFG *p, const char *str)');
    const MakePolymorphBFG = lib.func('void MakePolymorphBFG(int type, int x, double y, const char *str, _Out_ void *p)');
    const ReturnBigString = process.platform == 'win32' ?
                            lib.stdcall(1, koffi.disposable('str', koffi.free), ['str']) :
                            lib.func('const char * __stdcall ReturnBigString(const char *str)');
    const PrintFmt = lib.func('str_free PrintFmt(const char *fmt, ...)');
    const Concat16 = lib.func('const char16_t *! Concat16(const char16_t *str1, const char16_t *str2)')
    const ReturnFixedStr = lib.func('FixedString ReturnFixedStr(FixedString str)');
    const ReturnFixedStr2 = lib.func('FixedString2 ReturnFixedStr(FixedString2 str)');
    const ReturnFixedWide = lib.func('FixedWide ReturnFixedWide(FixedWide str)');
    const ReturnFixedWide2 = lib.func('FixedWide2 ReturnFixedWide(FixedWide2 str)');
    const ThroughUInt32UU = lib.func('uint32_t ThroughUInt32UU(uint32_t v)');
    const ThroughUInt32SS = lib.func('SingleU32 ThroughUInt32SS(SingleU32 s)');
    const ThroughUInt32SU = lib.func('SingleU32 ThroughUInt32SU(uint32_t v)');
    const ThroughUInt32US = lib.func('uint32_t ThroughUInt32US(SingleU32 s)');
    const ThroughUInt64UU = lib.func('uint64_t ThroughUInt64UU(uint64_t v)');
    const ThroughUInt64SS = lib.func('SingleU64 ThroughUInt64SS(SingleU64 s)');
    const ThroughUInt64SU = lib.func('SingleU64 ThroughUInt64SU(uint64_t v)');
    const ThroughUInt64US = lib.func('uint64_t ThroughUInt64US(SingleU64 s)');
    const ThroughInt64II = lib.func('int64_t ThroughInt64II(int64_t v)');
    const ThroughInt64SS = lib.func('SingleI64 ThroughInt64SS(SingleI64 s)');
    const ThroughInt64SI = lib.func('SingleI64 ThroughInt64SI(int64_t v)');
    const ThroughInt64IS = lib.func('int64_t ThroughInt64IS(SingleI64 s)');
    const ArrayToStruct = lib.func('IntContainer ArrayToStruct(int *ptr, int len)');
    const FillRange = lib.func('void FillRange(int init, int step, _Out_ int *out, int len)');
    const MultiplyIntegers = lib.func('void MultiplyIntegers(int multiplier, _Inout_ int *values, int len)');
    const ThroughStr = lib.func('str ThroughStr(StrStruct s)');
    const ThroughStr16 = lib.func('str16 ThroughStr16(StrStruct s)');
    const ReverseBytes = lib.func('void ReverseBytes(_Inout_ void *array, int len)');
    const CopyEndianInts1 = lib.func('void CopyEndianInts1(EndianInts ints, _Out_ uint8_t *buf)');
    const CopyEndianInts2 = lib.func('void CopyEndianInts2(int16_le_t i16le, int16_be_t i16be, uint16_le_t u16le, uint16_be_t u16be, ' +
                                                          'int32_le_t i32le, int32_be_t i32be, uint32_le_t u32le, uint32_be_t u32be, ' +
                                                          'int64_le_t i64le, int64_be_t i64be, uint64_le_t u64le, uint64_be_t u64be, ' +
                                                          '_Out_ void *out)');
    const ReturnEndianInt2SL = lib.func('int16_le_t ReturnEndianInt2(int16_be_t v)');
    const ReturnEndianInt2SB = lib.func('int16_be_t ReturnEndianInt2(int16_le_t v)');
    const ReturnEndianInt2UL = lib.func('uint16_le_t ReturnEndianInt2(uint16_be_t v)');
    const ReturnEndianInt2UB = lib.func('uint16_be_t ReturnEndianInt2(uint16_le_t v)');
    const ReturnEndianInt4SL = lib.func('int32_le_t ReturnEndianInt4(int32_be_t v)');
    const ReturnEndianInt4SB = lib.func('int32_be_t ReturnEndianInt4(int32_le_t v)');
    const ReturnEndianInt4UL = lib.func('uint32_le_t ReturnEndianInt4(uint32_be_t v)');
    const ReturnEndianInt4UB = lib.func('uint32_be_t ReturnEndianInt4(uint32_le_t v)');
    const ReturnEndianInt8SL = lib.func('int64_le_t ReturnEndianInt8(int64_be_t v)');
    const ReturnEndianInt8SB = lib.func('int64_be_t ReturnEndianInt8(int64_le_t v)');
    const ReturnEndianInt8UL = lib.func('uint64_le_t ReturnEndianInt8(uint64_be_t v)');
    const ReturnEndianInt8UB = lib.func('uint64_be_t ReturnEndianInt8(uint64_le_t v)');
    const ReverseBigText = lib.func('BigText ReverseBigText(BigText buf)');

    // Simple signed value returns
    assert.equal(GetMinusOne1(), -1);
    assert.equal(GetMinusOne2(), -1);
    assert.equal(GetMinusOne4(), -1);
    assert.equal(GetMinusOne8(null), -1);

    // Simple tests with Pack1
    {
        let p = {};

        FillPack1(777, p);
        assert.deepEqual(p, { a: 777 });

        let q = RetPack1(6);
        assert.deepEqual(q, { a: 6 });

        AddPack1(6, p);
        assert.deepEqual(p, { a: 783 });
    }

    // Simple tests with Pack2
    {
        let p = {};

        FillPack2(123, 456, p);
        assert.deepEqual(p, { a: 123, b: 456 });

        let q = RetPack2(6, 9);
        assert.deepEqual(q, { a: 6, b: 9 });

        AddPack2(6, 9, p);
        assert.deepEqual(p, { a: 129, b: 465 });
    }

    // Simple tests with Pack3
    {
        let p = {};

        FillPack3(1, 2, 3, p);
        assert.deepEqual(p, { a: 1, b: 2, c: 3 });

        let q = RetPack3(6, 9, -12);
        assert.deepEqual(q, { a: 6, b: 9, c: -12 });

        AddPack3(6, 9, -12, p);
        assert.deepEqual(p, { a: 7, b: 11, c: -9 });
    }

    // HFA tests
    {
        let f2p = {};
        let f2 = PackFloat2(1.5, 3.0, f2p);
        assert.deepEqual(f2, { a: 1.5, b: 3.0 });
        assert.deepEqual(f2, f2p);
        assert.deepEqual(ThroughFloat2({ a: 1.5, b: 3.0 }), f2);
        assert.deepEqual(ThroughFloat2(f2), f2);

        let f3p = {};
        let f3 = PackFloat3(20.0, 30.0, 40.0, f3p);
        assert.deepEqual(f3, { a: 20.0, b: Float32Array.from([30.0, 40.0]) });
        assert.deepEqual(f3, f3p);
        assert.deepEqual(ThroughFloat3({ a: 20.0, b: [30.0, 40.0] }), f3);
        assert.deepEqual(ThroughFloat3(f3), f3);

        let d2p = {};
        let d2 = PackDouble2(1.0, 2.0, d2p);
        assert.deepEqual(d2, { a: 1.0, b: 2.0 });
        assert.deepEqual(d2, d2p);

        let d3p = {};
        let d3 = PackDouble3(0.5, 10.0, 5.0, d3p);
        assert.deepEqual(d3, { a: 0.5, s: { b: 10.0, c: 5.0 } });
        assert.deepEqual(d3, d3p);
    }

    // Mixed int/float structs
    {
        let sif = { i: 4, f: 2.0 };
        assert.deepEqual(ReverseIntFloat(sif), { i: 2, f: 4 });
        assert.deepEqual(ReverseFloatInt(sif), { i: 2, f: 4 });
    }

    // Many parameters
    {
        assert.equal(ConcatenateToInt1(5, 6, 1, 2, 3, 9, 4, 4, 0, 6, 8, 7), 561239440687);
        assert.equal(ConcatenateToInt4(5, 6, 1, 2, 3, 9, 4, 4, 0, 6, 8, 7), 561239440687);
        assert.equal(ConcatenateToInt8(5, 6, 1, 2, 3, 9, 4, 4, 0, 6, 8, 7), 561239440687);
        assert.equal(ConcatenateToStr1(5, 6, 1, 2, 3, 9, 4, 4, {i: 0, j: 6, k: 8}, 7), '561239440687');
        assert.equal(ConcatenateToStr4(5, 6, 1, 2, 3, 9, 4, 4, {i: 0, j: 6, k: 8}, 7), '561239440687');
        assert.equal(ConcatenateToStr8(5, 6, 1, 2, 3, 9, 4, 4, {i: 0, j: 6, k: 8}, 7), '561239440687');
    }

    // Big struct
    {
        let out = {};
        let bfg = MakeBFG(out, 2, 7, '__Hello123456789++++foobarFOOBAR!__');
        assert.deepEqual(bfg, { a: 2, b: 4, c: -25, d: 'X/__Hello123456789++++foobarFOOBAR!__/X', e: 54, inner: { f: 14, g: 5 } });
        assert.deepEqual(out, bfg);
    }

    // Packed struct
    {
        let out = {};
        let bfg = MakePackedBFG(2, 7, out, '__Hello123456789++++foobarFOOBAR!__');
        assert.deepEqual(bfg, { a: 2, b: 4, c: -25, d: 'X/__Hello123456789++++foobarFOOBAR!__/X', e: 54, inner: { f: 14, g: 5 } });
        assert.deepEqual(out, bfg);
    }

    // Polymorph pointer
    {
        let bfg = {};

        MakePolymorphBFG(0, 2, 7, 'boo', koffi.as(bfg, 'BFG *'));
        assert.deepEqual(bfg, { a: 2, b: 4, c: -25, d: 'X/boo/X', e: 54, inner: { f: 14, g: 5 }});

        MakePolymorphBFG(1, 2, 7, 'bies', koffi.as(bfg, 'PackedBFG *'));
        assert.deepEqual(bfg, { a: 2, b: 4, c: -25, d: 'X/bies/X', e: 54, inner: { f: 14, g: 5 }});
    }

    // Big string
    {
        let str = 'fooBAR!'.repeat(1024 * 1024);
        assert.equal(ReturnBigString(str), str);
    }

    // Variadic
    {
        let str = PrintFmt('foo %d %g %s', 'int', 200, 'double', 1.5, 'str', 'BAR');
        assert.equal(str, 'foo 200 1.5 BAR');
    }

    // UTF-16LE strings
    {
        let str = Concat16('Hello ', 'World!');
        assert.equal(str, 'Hello World!');
    }

    // String to/from fixed-size buffers
    {
        let str = { buf: 'Hello!' };
        assert.deepEqual(ReturnFixedStr(str), { buf: Int8Array.from([72, 101, 108, 108, 111, 33, ...Array(58).fill(0)]) });
        assert.deepEqual(ReturnFixedStr2(str), { buf: 'Hello!' });
        assert.deepEqual(ReturnFixedWide(str), { buf: Int16Array.from([72, 101, 108, 108, 111, 33, ...Array(58).fill(0)]) });
        assert.deepEqual(ReturnFixedWide2(str), { buf: 'Hello!' });
    }

    // Big numbers
    {
        assert.strictEqual(ThroughUInt32UU(4294967284), 4294967284);
        assert.deepStrictEqual(ThroughUInt32SS({ v: 4294967284 }), { v: 4294967284 });
        assert.deepStrictEqual(ThroughUInt32SU(4294967284), { v: 4294967284 });
        assert.strictEqual(ThroughUInt32US({ v: 4294967284 }), 4294967284);

        assert.strictEqual(ThroughUInt64UU(9007199254740992n), 9007199254740992);
        assert.deepStrictEqual(ThroughUInt64SS({ v: 9007199254740992n }), { v: 9007199254740992 });
        assert.deepStrictEqual(ThroughUInt64SU(9007199254740992n), { v: 9007199254740992 });
        assert.strictEqual(ThroughUInt64US({ v: 9007199254740992n }), 9007199254740992);
        assert.strictEqual(ThroughUInt64UU(9007199254740993n), 9007199254740993n);
        assert.deepStrictEqual(ThroughUInt64SS({ v: 9007199254740993n }), { v: 9007199254740993n });
        assert.deepStrictEqual(ThroughUInt64SU(9007199254740993n), { v: 9007199254740993n });
        assert.strictEqual(ThroughUInt64US({ v: 9007199254740993n }), 9007199254740993n);
        assert.strictEqual(ThroughUInt64UU(18446744073709551598n), 18446744073709551598n);
        assert.deepStrictEqual(ThroughUInt64SS({ v: 18446744073709551598n }), { v: 18446744073709551598n });
        assert.deepStrictEqual(ThroughUInt64SU(18446744073709551598n), { v: 18446744073709551598n });
        assert.strictEqual(ThroughUInt64US({ v: 18446744073709551598n }), 18446744073709551598n);

        assert.strictEqual(ThroughInt64II(-9007199254740992), -9007199254740992);
        assert.deepStrictEqual(ThroughInt64SS({ v: -9007199254740992 }), { v: -9007199254740992 });
        assert.deepStrictEqual(ThroughInt64SI(-9007199254740992), { v: -9007199254740992 });
        assert.strictEqual(ThroughInt64IS({ v: -9007199254740992 }), -9007199254740992);
        assert.strictEqual(ThroughInt64II(-9007199254740993n), -9007199254740993n);
        assert.deepStrictEqual(ThroughInt64SS({ v: -9007199254740993n }), { v: -9007199254740993n });
        assert.deepStrictEqual(ThroughInt64SI(-9007199254740993n), { v: -9007199254740993n });
        assert.strictEqual(ThroughInt64IS({ v: -9007199254740993n }), -9007199254740993n);
        assert.strictEqual(ThroughInt64II(-9223372036854775803n), -9223372036854775803n);
        assert.deepStrictEqual(ThroughInt64SS({ v: -9223372036854775803n }), { v: -9223372036854775803n });
        assert.deepStrictEqual(ThroughInt64SI(-9223372036854775803n), { v: -9223372036854775803n });
        assert.strictEqual(ThroughInt64IS({ v: -9223372036854775803n }), -9223372036854775803n);
    }

    // Array pointers as input
    {
        let arr1 = [5, 7, 8, 4];
        let arr2 = Int32Array.from([8, 454, 6, 3, 45]);

        assert.deepEqual(ArrayToStruct(arr1, 0), { values: Int32Array.from(Array(16).fill(0)), len: 0 });
        assert.deepEqual(ArrayToStruct(arr1, 3), { values: Int32Array.from([5, 7, 8, ...Array(13).fill(0)]), len: 3 });
        assert.deepEqual(ArrayToStruct(arr2, 4), { values: Int32Array.from([8, 454, 6, 3, ...Array(12).fill(0)]), len: 4 });
        assert.deepEqual(ArrayToStruct(arr2, 2), { values: Int32Array.from([8, 454, ...Array(14).fill(0)]), len: 2 });
    }

    // Array pointers as output
    {
        let out1 = Array(10);
        let out2 = new Int32Array(10);
        let mult = -3;
        let ret = null;

        FillRange(2, 7, out1, out1.length);
        ret = FillRange(13, 3, out2, out2.length);

        assert.strictEqual(ret, undefined);
        assert.deepEqual(out1, [2, 9, 16, 23, 30, 37, 44, 51, 58, 65]);
        assert.deepEqual(out2, new Int32Array([13, 16, 19, 22, 25, 28, 31, 34, 37, 40]));

        MultiplyIntegers(-1, out1, out1.length - 2);
        MultiplyIntegers(3, out2, out2.length - 3);
        assert.deepEqual(out1, [-2, -9, -16, -23, -30, -37, -44, -51, 58, 65]);
        assert.deepEqual(out2, new Int32Array([3 * 13, 3 * 16, 3 * 19, 3 * 22, 3 * 25, 3 * 28, 3 * 31, 34, 37, 40]));
    }

    // Test struct strings
    {
        assert.equal(ThroughStr({ str: 'Hello', str16: null }), 'Hello');
        assert.equal(ThroughStr({ str: null, str16: 'Hello' }), null);
        assert.equal(ThroughStr16({ str: null, str16: 'World!' }), 'World!');
        assert.equal(ThroughStr16({ str: 'World!', str16: null }), null);
    }

    // Transparent typed arrays for void pointers
    {
        let arr8 = Uint8Array.from([1, 2, 3, 4, 5]);
        let arr16 = Int16Array.from([1, 2, 3, 4, 5]);

        ReverseBytes(arr8, arr8.byteLength);
        assert.deepEqual(arr8, Uint8Array.from([5, 4, 3, 2, 1]));

        ReverseBytes(arr16, arr16.byteLength);
        assert.deepEqual(arr16, Int16Array.from([1280, 1024, 768, 512, 256]));
    }

    // Endian-sensitive integer types
    {
        let ints = {
            i16le: 0x7BCD,
            i16be: 0x7BCD,
            u16le: 0x7BCD,
            u16be: 0x7BCD,
            i32le: 0x4EADBEEF,
            i32be: 0x4EADBEEF,
            u32le: 0x4EADBEEF,
            u32be: 0x4EADBEEF,
            i64le: 0x0123456789ABCDEFn,
            i64be: 0x0123456789ABCDEFn,
            u64le: 0x0123456789ABCDEFn,
            u64be: 0x0123456789ABCDEFn
        };

        let out1 = new Uint8Array(56);
        let out2 = new Uint8Array(56);

        CopyEndianInts1(ints, out1);
        CopyEndianInts2(ints.i16le, ints.i16be, ints.u16le, ints.u16be,
                        ints.i32le, ints.i32be, ints.u32le, ints.u32be,
                        ints.u64le, ints.u64be, ints.u64le, ints.u64be, out2);

        assert.deepEqual(out1, Uint8Array.from([
            0xCD, 0x7B, 0x7B, 0xCD,
            0xCD, 0x7B, 0x7B, 0xCD,
            0xEF, 0xBE, 0xAD, 0x4E, 0x4E, 0xAD, 0xBE, 0xEF,
            0xEF, 0xBE, 0xAD, 0x4E, 0x4E, 0xAD, 0xBE, 0xEF,
            0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
            0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF
        ]));
        assert.deepEqual(out2, out1);

        assert.equal(ReturnEndianInt2SL(0x7B6D), 0x6D7B);
        assert.equal(ReturnEndianInt2SB(0x7B6D), 0x6D7B);
        assert.equal(ReturnEndianInt2UL(0x7B6D), 0x6D7B);
        assert.equal(ReturnEndianInt2UB(0x7B6D), 0x6D7B);
        assert.equal(ReturnEndianInt4SL(0x4EADBE4F), 0x4FBEAD4E);
        assert.equal(ReturnEndianInt4SB(0x4EADBE4F), 0x4FBEAD4E);
        assert.equal(ReturnEndianInt4UL(0x4EADBE4F), 0x4FBEAD4E);
        assert.equal(ReturnEndianInt4UB(0x4EADBE4F), 0x4FBEAD4E);
        assert.equal(ReturnEndianInt8SL(0x0123456789ABCD3Fn), 0x3FCDAB8967452301n);
        assert.equal(ReturnEndianInt8SB(0x0123456789ABCD3Fn), 0x3FCDAB8967452301n);
        assert.equal(ReturnEndianInt8UL(0x0123456789ABCD3Fn), 0x3FCDAB8967452301n);
        assert.equal(ReturnEndianInt8UB(0x0123456789ABCD3Fn), 0x3FCDAB8967452301n);
    }

    // Test big structs
    {
        let text = 'hello!foo!bar'.repeat(20165);
        let expected = text.split('').reverse().join('');

        let big = { text: Buffer.from(text) };
        let ret = ReverseBigText(big);

        assert.equal(ret.len, big.len);
        assert.equal(Buffer.from(ret.text).toString(), expected);
    }
}
