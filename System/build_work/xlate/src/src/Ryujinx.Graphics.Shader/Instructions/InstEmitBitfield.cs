using Ryujinx.Graphics.Shader.Decoders;
using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.Translation;

using static Ryujinx.Graphics.Shader.Instructions.InstEmitHelper;
using static Ryujinx.Graphics.Shader.IntermediateRepresentation.OperandHelper;

namespace Ryujinx.Graphics.Shader.Instructions
{
    static partial class InstEmit
    {
        public static void BfeR(EmitterContext context)
        {
            InstBfeR op = context.GetOp<InstBfeR>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcReg(context, op.SrcB);

            EmitBfe(context, srcA, srcB, op.Dest, op.Brev, op.Signed);
        }

        public static void BfeI(EmitterContext context)
        {
            InstBfeI op = context.GetOp<InstBfeI>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcImm(context, Imm20ToSInt(op.Imm20));

            EmitBfe(context, srcA, srcB, op.Dest, op.Brev, op.Signed);
        }

        public static void BfeC(EmitterContext context)
        {
            InstBfeC op = context.GetOp<InstBfeC>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcCbuf(context, op.CbufSlot, op.CbufOffset);

            EmitBfe(context, srcA, srcB, op.Dest, op.Brev, op.Signed);
        }

        public static void BfiR(EmitterContext context)
        {
            InstBfiR op = context.GetOp<InstBfiR>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcReg(context, op.SrcB);
            Operand srcC = GetSrcReg(context, op.SrcC);

            EmitBfi(context, srcA, srcB, srcC, op.Dest);
        }

        public static void BfiI(EmitterContext context)
        {
            InstBfiI op = context.GetOp<InstBfiI>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcImm(context, Imm20ToSInt(op.Imm20));
            Operand srcC = GetSrcReg(context, op.SrcC);

            EmitBfi(context, srcA, srcB, srcC, op.Dest);
        }

        public static void BfiC(EmitterContext context)
        {
            InstBfiC op = context.GetOp<InstBfiC>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcCbuf(context, op.CbufSlot, op.CbufOffset);
            Operand srcC = GetSrcReg(context, op.SrcC);

            EmitBfi(context, srcA, srcB, srcC, op.Dest);
        }

        public static void BfiRc(EmitterContext context)
        {
            InstBfiRc op = context.GetOp<InstBfiRc>();

            Operand srcA = GetSrcReg(context, op.SrcA);
            Operand srcB = GetSrcReg(context, op.SrcC);
            Operand srcC = GetSrcCbuf(context, op.CbufSlot, op.CbufOffset);

            EmitBfi(context, srcA, srcB, srcC, op.Dest);
        }

        public static void FloR(EmitterContext context)
        {
            InstFloR op = context.GetOp<InstFloR>();

            EmitFlo(context, GetSrcReg(context, op.SrcB), op.Dest, op.NegB, op.Sh, op.Signed);
        }

        public static void FloI(EmitterContext context)
        {
            InstFloI op = context.GetOp<InstFloI>();

            EmitFlo(context, GetSrcImm(context, Imm20ToSInt(op.Imm20)), op.Dest, op.NegB, op.Sh, op.Signed);
        }

        public static void FloC(EmitterContext context)
        {
            InstFloC op = context.GetOp<InstFloC>();

            EmitFlo(context, GetSrcCbuf(context, op.CbufSlot, op.CbufOffset), op.Dest, op.NegB, op.Sh, op.Signed);
        }

        public static void PopcR(EmitterContext context)
        {
            InstPopcR op = context.GetOp<InstPopcR>();

            EmitPopc(context, GetSrcReg(context, op.SrcB), op.Dest, op.NegB);
        }

        public static void PopcI(EmitterContext context)
        {
            InstPopcI op = context.GetOp<InstPopcI>();

            EmitPopc(context, GetSrcImm(context, Imm20ToSInt(op.Imm20)), op.Dest, op.NegB);
        }

        public static void PopcC(EmitterContext context)
        {
            InstPopcC op = context.GetOp<InstPopcC>();

            EmitPopc(context, GetSrcCbuf(context, op.CbufSlot, op.CbufOffset), op.Dest, op.NegB);
        }

        private static void EmitBfe(
            EmitterContext context,
            Operand srcA,
            Operand srcB,
            int rd,
            bool bitReverse,
            bool isSigned)
        {
            if (bitReverse)
            {
                srcA = context.BitfieldReverse(srcA);
            }

            Operand position = context.BitwiseAnd(srcB, Const(0xff));

            Operand size = context.BitfieldExtractU32(srcB, Const(8), Const(8));

            // TOTK: Maxwell's BFE (as PTX bfe) cuts a field that runs past bit 31 there, and a field that starts past it
            // gives 0, or bit 31 repeated when signed and the size is not 0. SPIR-V leaves an offset or offset + count over
            // 32 undefined, and AMD's bitfield instructions take offset and width modulo 32. TOTK's Depths terrain vertex
            // shaders take position and size from a register (BFE.R), so they are clamped into the defined range here;
            // constant fields inside the word are emitted as before.
            bool constantInRange = srcB.Type == OperandType.Constant && (srcB.Value & 0xff) + ((srcB.Value >> 8) & 0xff) <= 32;
            Operand positionIn = position;
            Operand sizeIn = size;

            if (!constantInRange)
            {
                ClampBitfield(context, ref position, ref size);
            }

            Operand res = isSigned
                ? context.BitfieldExtractS32(srcA, position, size)
                : context.BitfieldExtractU32(srcA, position, size);

            if (!constantInRange && isSigned)
            {
                // The clamped extract gave 0 for a field that starts past bit 31; a signed one is bit 31 repeated.
                Operand signBits = context.ConditionalSelect(context.ICompareEqual(sizeIn, Const(0)), Const(0), context.ShiftRightS32(srcA, Const(31)));

                res = context.ConditionalSelect(context.ICompareGreaterOrEqualUnsigned(positionIn, Const(32)), signBits, res);
            }

            context.Copy(GetDest(rd), res);

            // TODO: CC, X.
        }

        // TOTK: position and size of a bitfield cut to the 32-bit word: the size shrinks to what is left of the word
        // after the position (0 for a position of 32 or more) and the position stays at most 31, so offset and
        // offset + count stay inside the range SPIR-V defines.
        private static void ClampBitfield(EmitterContext context, ref Operand position, ref Operand size)
        {
            Operand left = context.ISubtract(Const(32), context.IMinimumU32(position, Const(32)));

            size = context.IMinimumU32(size, left);
            position = context.IMinimumU32(position, Const(31));
        }

        private static void EmitBfi(EmitterContext context, Operand srcA, Operand srcB, Operand srcC, int rd)
        {
            Operand position = context.BitwiseAnd(srcB, Const(0xff));

            Operand size = context.BitfieldExtractU32(srcB, Const(8), Const(8));

            // TOTK: as in EmitBfe, a field past bit 31 is cut there; one that starts past it inserts nothing (size 0
            // after the clamp), so C comes back unchanged, as on Maxwell.
            if (!(srcB.Type == OperandType.Constant && (srcB.Value & 0xff) + ((srcB.Value >> 8) & 0xff) <= 32))
            {
                ClampBitfield(context, ref position, ref size);
            }

            Operand res = context.BitfieldInsert(srcC, srcA, position, size);

            context.Copy(GetDest(rd), res);
        }

        private static void EmitFlo(EmitterContext context, Operand src, int rd, bool invert, bool sh, bool isSigned)
        {
            Operand srcB = context.BitwiseNot(src, invert);

            Operand res;

            if (sh)
            {
                res = context.FindLSB(context.BitfieldReverse(srcB));
            }
            else
            {
                res = isSigned
                    ? context.FindMSBS32(srcB)
                    : context.FindMSBU32(srcB);
            }

            context.Copy(GetDest(rd), res);
        }

        private static void EmitPopc(EmitterContext context, Operand src, int rd, bool invert)
        {
            Operand srcB = context.BitwiseNot(src, invert);

            Operand res = context.BitCount(srcB);

            context.Copy(GetDest(rd), res);
        }
    }
}
