#pragma once

// Software binary64 for GPUs with shaderInt64 but without shaderFloat64.
// Representation is the original IEEE-754 uint64 bit pattern. All arithmetic
// and conversions use SPIR-V integer operations; no OpTypeFloat 64 is emitted.
// Scoped by SpirvEmitter to PPSA24156 compute hash becd439e2fa1efd1.
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter::Soft64 {
inline uint32_t K(EmitterState& s, uint64_t v) { return ConstantU64(s,v); }
inline uint32_t K32(EmitterState& s, uint32_t v) { return ConstantU32(s,v); }
inline uint32_t B(EmitterState& s, spv::Op op, uint32_t a,uint32_t b) {
    return Binary(s,op,TypeU64(s),a,b);
}
inline uint32_t A(EmitterState& s,uint32_t a,uint32_t b){return B(s,spv::OpIAdd,a,b);}
inline uint32_t D(EmitterState& s,uint32_t a,uint32_t b){return B(s,spv::OpISub,a,b);}
inline uint32_t And(EmitterState& s,uint32_t a,uint32_t b){return B(s,spv::OpBitwiseAnd,a,b);}
inline uint32_t Or(EmitterState& s,uint32_t a,uint32_t b){return B(s,spv::OpBitwiseOr,a,b);}
inline uint32_t Shl(EmitterState& s,uint32_t a,uint32_t n){return B(s,spv::OpShiftLeftLogical,a,n);}
inline uint32_t Shr(EmitterState& s,uint32_t a,uint32_t n){return B(s,spv::OpShiftRightLogical,a,n);}
inline uint32_t Cmp(EmitterState& s, spv::Op op,uint32_t a,uint32_t b) {
    return Binary(s,op,TypeBool(s),a,b);
}
inline uint32_t Eq(EmitterState& s,uint32_t a,uint32_t b){return Cmp(s,spv::OpIEqual,a,b);}
inline uint32_t Ne(EmitterState& s,uint32_t a,uint32_t b){return Cmp(s,spv::OpINotEqual,a,b);}
inline uint32_t Lt(EmitterState& s,uint32_t a,uint32_t b){return Cmp(s,spv::OpULessThan,a,b);}
inline uint32_t Gt(EmitterState& s,uint32_t a,uint32_t b){return Cmp(s,spv::OpUGreaterThan,a,b);}
inline uint32_t Bool(EmitterState& s,spv::Op op,uint32_t a,uint32_t b) {
    return Binary(s,op,TypeBool(s),a,b);
}
inline uint32_t Sel(EmitterState& s,uint32_t cond,uint32_t yes,uint32_t no) {
    return Select(s,TypeU64(s),cond,yes,no);
}
inline uint32_t Sign(EmitterState& s,uint32_t x){return And(s,x,K(s,0x8000000000000000ull));}
inline uint32_t Exp(EmitterState& s,uint32_t x){return And(s,Shr(s,x,K32(s,52)),K(s,2047));}
inline uint32_t Frac(EmitterState& s,uint32_t x){return And(s,x,K(s,0x000fffffffffffffull));}
inline uint32_t IsNan(EmitterState& s,uint32_t e,uint32_t f) {
    return Bool(s,spv::OpLogicalAnd,Eq(s,e,K(s,2047)),Ne(s,f,K(s,0)));
}
inline uint32_t IsInf(EmitterState& s,uint32_t e,uint32_t f) {
    return Bool(s,spv::OpLogicalAnd,Eq(s,e,K(s,2047)),Eq(s,f,K(s,0)));
}
inline uint32_t QuietNan(EmitterState& s) { return K(s,0x7ff8000000000000ull); }
inline uint32_t ShiftJam(EmitterState& s,uint32_t value,uint32_t count) {
    const auto high=Gt(s,count,K(s,63));
    const auto n=Sel(s,high,K(s,63),count);
    const auto shifted=Shr(s,value,n);
    const auto mask=D(s,Shl(s,K(s,1),n),K(s,1));
    const auto sticky=Ne(s,And(s,value,mask),K(s,0));
    const auto jam=Or(s,shifted,Sel(s,sticky,K(s,1),K(s,0)));
    return Sel(s,high,Sel(s,Ne(s,value,K(s,0)),K(s,1),K(s,0)),jam);
}
// Normalize nonzero mantissa to the designated leading bit, subtracting the
// shifts from a signed (two's complement) biased exponent.
inline void Normalize(EmitterState& s,uint32_t& sig,uint32_t& exp,uint32_t target) {
    for (uint32_t n: {32u,16u,8u,4u,2u,1u}) {
        if(n>target)continue;
        const auto shift=Bool(s,spv::OpLogicalAnd,Ne(s,sig,K(s,0)),
                            Lt(s,sig,K(s,1ull << (target-n+1u))));
        sig=Sel(s,shift,Shl(s,sig,K32(s,n)),sig);
        exp=Sel(s,shift,D(s,exp,K(s,n)),exp);
    }
}
inline uint32_t RoundPack(EmitterState& s,uint32_t sign,uint32_t sig,uint32_t exp) {
    // exp is a signed biased exponent in a uint64 SPIR-V register.
    const auto sub=Cmp(s,spv::OpSLessThan,exp,K(s,1));
    const auto sub_shift=D(s,K(s,1),exp);
    sig=Sel(s,sub,ShiftJam(s,sig,sub_shift),sig);
    exp=Sel(s,sub,K(s,1),exp);
    const auto mant=Shr(s,sig,K32(s,3));
    const auto low=And(s,sig,K(s,7));
    const auto round_up=Bool(s,spv::OpLogicalOr,Gt(s,low,K(s,4)),
                            Bool(s,spv::OpLogicalAnd,Eq(s,low,K(s,4)),
                                 Ne(s,And(s,mant,K(s,1)),K(s,0))));
    auto rounded=A(s,mant,Sel(s,round_up,K(s,1),K(s,0)));
    const auto carry=Ne(s,And(s,rounded,K(s,1ull<<53)),K(s,0));
    rounded=Sel(s,carry,Shr(s,rounded,K32(s,1)),rounded);
    exp=A(s,exp,Sel(s,carry,K(s,1),K(s,0)));
    const auto normal=Bool(s,spv::OpLogicalOr,Gt(s,exp,K(s,1)),
                            Bool(s,spv::OpLogicalAnd,Eq(s,exp,K(s,1)),
                                 Ne(s,And(s,rounded,K(s,1ull<<52)),K(s,0))));
    const auto field=Sel(s,normal,exp,K(s,0));
    const auto finite=Or(s,Or(s,sign,Shl(s,field,K32(s,52))),
                         And(s,rounded,K(s,0x000fffffffffffffull)));
    const auto overflow=Cmp(s,spv::OpSGreaterThanEqual,exp,K(s,2047));
    return Sel(s,overflow,Or(s,sign,K(s,0x7ff0000000000000ull)),finite);
}
inline uint32_t FromFloat(EmitterState& s,uint32_t value) {
    const auto bits=Unary(s,spv::OpBitcast,TypeU32(s),value);
    const auto wide=Unary(s,spv::OpUConvert,TypeU64(s),bits);
    const auto sign=Shl(s,And(s,wide,K(s,0x80000000)),K32(s,32));
    const auto e=And(s,Shr(s,wide,K32(s,23)),K(s,255));
    const auto frac=And(s,wide,K(s,0x7fffff));
    auto exp=Sel(s,Eq(s,e,K(s,0)),K(s,897),A(s,e,K(s,896)));
    auto sig=frac;
    const auto sub=Eq(s,e,K(s,0));
    Normalize(s,sig,exp,23);
    // For normals the original mantissa already has a leading implicit bit.
    // Normalization above only changes nonzero subnormal significands.
    const auto field=Sel(s,sub,exp,A(s,e,K(s,896)));
    const auto mant=Sel(s,sub,And(s,sig,K(s,0x7fffff)),frac);
    const auto finite=Or(s,sign,Or(s,Shl(s,field,K32(s,52)),Shl(s,mant,K32(s,29))));
    const auto zero=Eq(s,frac,K(s,0));
    const auto result=Sel(s,Bool(s,spv::OpLogicalAnd,sub,zero),sign,finite);
    const auto is_special=Eq(s,e,K(s,255));
    const auto special=Or(s,sign,Or(s,K(s,0x7ff0000000000000ull),
                        Shl(s,frac,K32(s,29))));
    return Sel(s,is_special,Sel(s,zero,special,Or(s,special,K(s,1ull<<51))),result);
}
inline uint32_t ToFloat(EmitterState& s,uint32_t bits) {
    const auto sign=Shr(s,Sign(s,bits),K32(s,32));
    const auto e=Exp(s,bits), f=Frac(s,bits);
    const auto ef=D(s,e,K(s,896)); // signed exponent in binary32 bias
    const auto sub=Cmp(s,spv::OpSLessThan,ef,K(s,1));
    const auto effective=Sel(s,sub,K(s,1),ef);
    const auto significand=Or(s,f,K(s,1ull<<52));
    const auto shift=A(s,K(s,29),Sel(s,sub,D(s,K(s,1),ef),K(s,0)));
    const auto sig=ShiftJam(s,Shl(s,significand,K32(s,3)),shift);
    const auto mant=Shr(s,sig,K32(s,3)),lo=And(s,sig,K(s,7));
    const auto up=Bool(s,spv::OpLogicalOr,Gt(s,lo,K(s,4)),
                       Bool(s,spv::OpLogicalAnd,Eq(s,lo,K(s,4)),Ne(s,And(s,mant,K(s,1)),K(s,0))));
    auto rounded=A(s,mant,Sel(s,up,K(s,1),K(s,0)));
    const auto carry=Ne(s,And(s,rounded,K(s,1ull<<24)),K(s,0));
    rounded=Sel(s,carry,Shr(s,rounded,K32(s,1)),rounded);
    const auto exp_after=A(s,effective,Sel(s,carry,K(s,1),K(s,0)));
    const auto is_normal=Bool(s,spv::OpLogicalOr,Cmp(s,spv::OpSGreaterThan,ef,K(s,0)),
                             Ne(s,And(s,rounded,K(s,1ull<<23)),K(s,0)));
    const auto out_exp=Sel(s,is_normal,exp_after,K(s,0));
    auto finite=Or(s,sign,Or(s,Shl(s,out_exp,K32(s,23)),And(s,rounded,K(s,0x7fffff))));
    // Existing Kyty conversion saturates finite overflow at max finite F32.
    const auto overflow=Cmp(s,spv::OpSGreaterThanEqual,ef,K(s,255));
    finite=Sel(s,overflow,Or(s,sign,K(s,0x7f7fffff)),finite);
    // All binary64 subnormals are too small to be represented in binary32.
    finite=Sel(s,Eq(s,e,K(s,0)),sign,finite);
    const auto special=Or(s,sign,Sel(s,Eq(s,f,K(s,0)),K(s,0x7f800000),K(s,0x7fc00000)));
    const auto out=Sel(s,Eq(s,e,K(s,2047)),special,finite);
    const auto narrow=Unary(s,spv::OpUConvert,TypeU32(s),out);
    return Unary(s,spv::OpBitcast,TypeF32(s),narrow);
}
inline uint32_t Add(EmitterState& s,uint32_t x,uint32_t y) {
    const auto sa=Sign(s,x),sb=Sign(s,y),ea=Exp(s,x),eb=Exp(s,y);
    const auto fa=Frac(s,x),fb=Frac(s,y);
    const auto ma=Or(s,fa,Sel(s,Ne(s,ea,K(s,0)),K(s,1ull<<52),K(s,0)));
    const auto mb=Or(s,fb,Sel(s,Ne(s,eb,K(s,0)),K(s,1ull<<52),K(s,0)));
    const auto ax=Sel(s,Eq(s,ea,K(s,0)),K(s,1),ea);
    const auto bx=Sel(s,Eq(s,eb,K(s,0)),K(s,1),eb);
    const auto a_big=Bool(s,spv::OpLogicalOr,Gt(s,ax,bx),
                         Bool(s,spv::OpLogicalAnd,Eq(s,ax,bx),Cmp(s,spv::OpUGreaterThanEqual,ma,mb)));
    const auto bigexp=Sel(s,a_big,ax,bx);
    const auto smallexp=Sel(s,a_big,bx,ax);
    const auto big=Shl(s,Sel(s,a_big,ma,mb),K32(s,3));
    const auto small=ShiftJam(s,Shl(s,Sel(s,a_big,mb,ma),K32(s,3)),D(s,bigexp,smallexp));
    const auto same=Eq(s,sa,sb);
    const auto sum=A(s,big,small);
    const auto delta=D(s,big,small);
    const auto carry=Ne(s,And(s,sum,K(s,1ull<<56)),K(s,0));
    auto sig=Sel(s,same,Sel(s,carry,ShiftJam(s,sum,K(s,1)),sum),delta);
    auto exponent=A(s,bigexp,Sel(s,Bool(s,spv::OpLogicalAnd,same,carry),K(s,1),K(s,0)));
    Normalize(s,sig,exponent,55);
    const auto result=RoundPack(s,Sel(s,a_big,sa,sb),sig,exponent);
    const auto both_zero=Bool(s,spv::OpLogicalAnd,Eq(s,ma,K(s,0)),Eq(s,mb,K(s,0)));
    const auto cancel=Bool(s,spv::OpLogicalAnd,Ne(s,sa,sb),Eq(s,delta,K(s,0)));
    const auto zero=Sel(s,both_zero,And(s,sa,sb),K(s,0));
    const auto finite=Sel(s,Bool(s,spv::OpLogicalOr,both_zero,cancel),zero,result);
    const auto na=IsNan(s,ea,fa),nb=IsNan(s,eb,fb);
    const auto ia=IsInf(s,ea,fa),ib=IsInf(s,eb,fb);
    const auto infinity=Sel(s,ia,Or(s,sa,K(s,0x7ff0000000000000ull)),
                                    Or(s,sb,K(s,0x7ff0000000000000ull)));
    const auto invalid=Bool(s,spv::OpLogicalAnd,Bool(s,spv::OpLogicalAnd,ia,ib),Ne(s,sa,sb));
    const auto special=Sel(s,invalid,QuietNan(s),infinity);
    return Sel(s,Bool(s,spv::OpLogicalOr,na,nb),QuietNan(s),
               Sel(s,Bool(s,spv::OpLogicalOr,ia,ib),special,finite));
}
inline uint32_t Mul(EmitterState& s,uint32_t x,uint32_t y) {
    const auto sa=Sign(s,x),sb=Sign(s,y);
    const auto signed_result=B(s,spv::OpBitwiseXor,sa,sb);
    const auto ea=Exp(s,x),eb=Exp(s,y),fa=Frac(s,x),fb=Frac(s,y);
    const auto ma=Or(s,fa,Sel(s,Ne(s,ea,K(s,0)),K(s,1ull<<52),K(s,0)));
    const auto mb=Or(s,fb,Sel(s,Ne(s,eb,K(s,0)),K(s,1ull<<52),K(s,0)));
    auto aa=ma,bb=mb;
    auto xa=Sel(s,Eq(s,ea,K(s,0)),K(s,1),ea);
    auto xb=Sel(s,Eq(s,eb,K(s,0)),K(s,1),eb);
    Normalize(s,aa,xa,52);
    Normalize(s,bb,xb,52);
    const auto pair_type=s.builder.Type(spv::OpTypeStruct,TypeU64(s),TypeU64(s));
    const auto extended=s.builder.AllocateId();
    s.builder.AddFunction(spv::OpUMulExtended,pair_type,extended,aa,bb);
    const auto lo=s.builder.AllocateId(),hi=s.builder.AllocateId();
    s.builder.AddFunction(spv::OpCompositeExtract,TypeU64(s),lo,extended,0);
    s.builder.AddFunction(spv::OpCompositeExtract,TypeU64(s),hi,extended,1);
    const auto high=Ne(s,And(s,hi,K(s,1ull<<41)),K(s,0));
    const auto n=Sel(s,high,K(s,50),K(s,49));
    const auto shr=Shr(s,lo,n);
    const auto shl=Shl(s,hi,D(s,K(s,64),n));
    const auto mask=D(s,Shl(s,K(s,1),n),K(s,1));
    const auto sticky=Ne(s,And(s,lo,mask),K(s,0));
    const auto sig=Or(s,Or(s,shr,shl),Sel(s,sticky,K(s,1),K(s,0)));
    const auto exponent=A(s,D(s,A(s,xa,xb),K(s,1023)),Sel(s,high,K(s,1),K(s,0)));
    const auto finite=RoundPack(s,signed_result,sig,exponent);
    const auto zero=Bool(s,spv::OpLogicalOr,Eq(s,ma,K(s,0)),Eq(s,mb,K(s,0)));
    const auto ia=IsInf(s,ea,fa),ib=IsInf(s,eb,fb);
    const auto na=IsNan(s,ea,fa),nb=IsNan(s,eb,fb);
    const auto invalid=Bool(s,spv::OpLogicalAnd,Bool(s,spv::OpLogicalOr,ia,ib),zero);
    const auto infinity=Or(s,signed_result,K(s,0x7ff0000000000000ull));
    return Sel(s,Bool(s,spv::OpLogicalOr,na,nb),QuietNan(s),
               Sel(s,invalid,QuietNan(s),
                   Sel(s,Bool(s,spv::OpLogicalOr,ia,ib),infinity,
                       Sel(s,zero,signed_result,finite))));
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter::Soft64
