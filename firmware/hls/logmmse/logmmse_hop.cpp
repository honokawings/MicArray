#include <hls_fft.h>
#include <ap_fixed.h>
#include <cmath>
#include <cstdint>
#include <complex>
#include "logmmse_scalar_template.h"
#include "hann2048_double.h"

static const int N=2048, HOP=1024, BANK=513;
typedef ap_fixed<32,1> fft_input_t;
typedef ap_fixed<48,17> fft_output_t;
struct fft_params: hls::ip_fft::params_t {
    static const unsigned input_width=32,output_width=48,max_nfft=11;
    static const unsigned config_width=8,status_width=8;
    static const unsigned arch_opt=hls::ip_fft::radix_4_burst_io;
    static const unsigned phase_factor_width=34;
    static const unsigned ordering_opt=hls::ip_fft::natural_order;
    static const unsigned scaling_opt=hls::ip_fft::unscaled;
    static const unsigned rounding_opt=hls::ip_fft::convergent_rounding;
    static const unsigned stages_block_ram=0;
    static const bool ovflo=false;
};

static double binary_scale(int exponent) {
#pragma HLS INLINE
    union { uint64_t u; double d; } item;
    item.u=uint64_t(exponent+1023)<<52;
    return item.d;
}
static double exact_ldexp(double value,int delta,bool &invalid) {
#pragma HLS INLINE
    union {uint64_t u;double d;} item;item.d=value;
    const unsigned exponent=unsigned((item.u>>52)&2047);
    if((item.u&0x7fffffffffffffffULL)==0)return value;
    const int target=int(exponent)+delta;
    if(exponent==0 || exponent==2047 || target<=0 || target>=2047) {
        invalid=true;return 0;
    }
    item.u=(item.u&0x800fffffffffffffULL)|(uint64_t(target)<<52);
    return item.d;
}
static int exponent_of(double value) {
#pragma HLS INLINE
    union { uint64_t u; double d; } item;
    item.d=value;
    return value==0.0 ? 0 : int((item.u>>52)&2047)-1023;
}
static fft_input_t quantize_input(double value) {
#pragma HLS INLINE
    // Exact round-to-nearest-even Q31 conversion for finite |value| <= 0.5.
    // Avoid the general ap_fixed<double> converter's 1075-bit shift network.
    union { uint64_t u; double d; } bits;bits.d=value;
    const bool negative=bool(bits.u>>63);
    const int exponent=int((bits.u>>52)&2047);
    uint64_t rounded=0;
    const int shift=1044-exponent;
    if(exponent && shift>0 && shift<54) {
        const uint64_t significand=(uint64_t(1)<<52)|(bits.u&0xfffffffffffffULL);
        rounded=significand>>shift;
        const uint64_t remainder=significand&((uint64_t(1)<<shift)-1);
        const uint64_t half=uint64_t(1)<<(shift-1);
        if(remainder>half || (remainder==half && (rounded&1))) ++rounded;
    }
    const int64_t signed_value=negative?-int64_t(rounded):int64_t(rounded);
    ap_int<32> raw=signed_value;
    fft_input_t result;result.range(31,0)=raw.range(31,0);return result;
}
static ap_uint<64> pack_input(fft_input_t real,fft_input_t imag) {
#pragma HLS INLINE
    ap_uint<64> item;item.range(31,0)=real.range(31,0);item.range(63,32)=imag.range(31,0);return item;
}
static double unpack_real(ap_uint<96> value) {
#pragma HLS INLINE
    ap_int<48> raw;raw.range(47,0)=value.range(47,0);
    const int64_t integer=raw.to_int64();
    const bool negative=integer<0;
    const uint64_t magnitude=negative?uint64_t(-integer):uint64_t(integer);
    int leading=0;
    for(int bit=0;bit<48;++bit) {
#pragma HLS UNROLL
        if((magnitude>>bit)&1)leading=bit;
    }
    union {uint64_t u;double d;} result;
    result.u=magnitude?((uint64_t(negative)<<63)|(uint64_t(leading+992)<<52)|((magnitude<<(52-leading))&0xfffffffffffffULL)):0;
    return result.d;
}
static double unpack_imag(ap_uint<96> value) {
#pragma HLS INLINE
    ap_uint<96> shifted=0;shifted.range(47,0)=value.range(95,48);return unpack_real(shifted);
}
// Explicit packed bridge avoids the 2020.2 std::complex unscaled template bug
// and the obsolete x_complex DATA_PACK port mismatch. Vendor sources untouched.
static void load_fft(ap_uint<64> input[N],ap_uint<64> fifo[N]) {
#pragma HLS INLINE off
    for(int i=0;i<N;++i) {
#pragma HLS PIPELINE II=1
        fifo[i]=input[i];
    }
}
static void unload_fft(ap_uint<96> fifo[N],ap_uint<96> output[N]) {
#pragma HLS INLINE off
    for(int i=0;i<N;++i) {
#pragma HLS PIPELINE II=1
        output[i]=fifo[i];
    }
}
static void fft_engine(ap_uint<64> input[N],ap_uint<96> output[N],bool inverse) {
#pragma HLS INLINE off
    hls::ip_fft::config_t<fft_params> config;
    hls::ip_fft::status_t<fft_params> status;
    config.setDir(!inverse);
#ifdef AESL_SYN
#pragma HLS DATAFLOW
    hls::stream<hls::ip_fft::config_t<fft_params> > config_stream;
    hls::stream<hls::ip_fft::status_t<fft_params> > status_stream;
    ap_uint<64> packed_in[N];
    ap_uint<96> packed_out[N];
#pragma HLS STREAM variable=packed_in depth=16
#pragma HLS STREAM variable=packed_out depth=16
    config_stream.write(config);
    load_fft(input,packed_in);
    hls::fft_syn<fft_params>(packed_in,packed_out,status_stream,config_stream);
    unload_fft(packed_out,output);
    status=status_stream.read();
#else
    hls::x_complex<fft_input_t> in_array[N];
    hls::x_complex<fft_output_t> out_array[N];
    for(int i=0;i<N;++i) {
        fft_input_t re,im;re.range(31,0)=input[i].range(31,0);im.range(31,0)=input[i].range(63,32);
        in_array[i]=hls::x_complex<fft_input_t>(re,im);
    }
    hls::fft<fft_params>(in_array,out_array,&status,&config);
    for(int i=0;i<N;++i) {
        output[i].range(47,0)=out_array[i].real().range(47,0);
        output[i].range(95,48)=out_array[i].imag().range(47,0);
    }
#endif
}

template<int LANE> static void apply_lane(double power[BANK],double noise[BANK],double gamma[BANK],double gain[BANK],double alpha,double beta,double min_gain,unsigned &fault) {
#pragma HLS INLINE off
    unsigned errors=0;
    const int COUNT=LANE==1?512:513;
    for(int i=0;i<COUNT;++i) {
#pragma HLS LOOP_TRIPCOUNT min=512 max=513
        double ng,nn,npg;bool failed;
        scalar_bin<LANE>(power[i],noise[i],gamma[i],gain[i],alpha,beta,min_gain,ng,nn,npg,failed);
        if(failed)errors|=1;
        noise[i]=nn;gamma[i]=npg;gain[i]=ng;
    }
    fault=errors;
}
static void apply_three(double power[2][BANK],double noise[2][BANK],double gamma[2][BANK],double gain[2][BANK],double alpha,double beta,double min_gain,unsigned &fault) {
#pragma HLS INLINE off
// DATAFLOW disabled for compiler isolation
#pragma HLS ARRAY_PARTITION variable=power complete dim=1
#pragma HLS ARRAY_PARTITION variable=noise complete dim=1
#pragma HLS ARRAY_PARTITION variable=gamma complete dim=1
#pragma HLS ARRAY_PARTITION variable=gain complete dim=1
    unsigned f0,f1;
    apply_lane<0>(power[0],noise[0],gamma[0],gain[0],alpha,beta,min_gain,f0);
    apply_lane<1>(power[1],noise[1],gamma[1],gain[1],alpha,beta,min_gain,f1);
    fault=f0|f1;
}

// One call consumes and emits exactly 1024 samples. Two initial zero hops are
// the source's 2048-entry initial output queue. No EOF flush is performed.
void logmmse_hop(bool reset_state,float samples[HOP],float result[HOP],double alpha,double beta,double min_gain,unsigned &numeric_fault) {
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS INTERFACE ap_memory port=samples
#pragma HLS INTERFACE ap_memory port=result
#pragma HLS ALLOCATION function instances=fft_engine limit=1
#pragma HLS ALLOCATION operation instances=dmul limit=2
#pragma HLS ALLOCATION operation instances=dadd limit=2
#pragma HLS ALLOCATION operation instances=dsub limit=2
    static bool have_half=false,have_pending=false,noise_initialized=false;
    static unsigned fault_state=0;
    static float ring[N],pending[HOP];
    static double tail[HOP];
    static double noise[2][BANK],gamma[2][BANK],gain[2][BANK];
    double windowed[N],power[2][BANK];
    double reduced_real[1025],reduced_imag[1025];
    ap_uint<64> fft_input[N];
    ap_uint<96> fft_output[N];
#pragma HLS BIND_STORAGE variable=ring type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=pending type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=tail type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=noise type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=gamma type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=gain type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=windowed type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=power type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=reduced_real type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=reduced_imag type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=fft_input type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=fft_output type=ram_1p impl=lutram
#pragma HLS BIND_STORAGE variable=HANN type=rom_1p impl=lutram
#pragma HLS ARRAY_PARTITION variable=noise complete dim=1
#pragma HLS ARRAY_PARTITION variable=gamma complete dim=1
#pragma HLS ARRAY_PARTITION variable=gain complete dim=1
#pragma HLS ARRAY_PARTITION variable=power complete dim=1
    if(reset_state) {
        have_half=false;have_pending=false;noise_initialized=false;fault_state=0;numeric_fault=0;
        for(int i=0;i<HOP;++i) {tail[i]=0;pending[i]=0;result[i]=0;}
        for(int i=0;i<BANK;++i) {
            for(int lane=0;lane<2;++lane) {
#pragma HLS UNROLL
                noise[lane][i]=1e-12;gamma[lane][i]=1;gain[lane][i]=1;
            }
        }
        return;
    }
    for(int i=0;i<HOP;++i) {
        result[i]=have_pending?pending[i]:0.0f;
        ring[i]=ring[i+HOP];ring[i+HOP]=samples[i];
    }
    if(!have_half) {have_half=true;numeric_fault=fault_state;return;}
    double maximum=0;
    for(int i=0;i<N;++i) {
        const double value=static_cast<double>(ring[i])*HANN[i];
        windowed[i]=value;
        if(std::fabs(value)>maximum) maximum=std::fabs(value);
    }
    // One magnitude headroom bit avoids rounding overflow at a binary boundary.
    const int first_exp=exponent_of(maximum);
    bool exponent_fault=false;
    for(int i=0;i<N;++i) fft_input[i]=pack_input(quantize_input(exact_ldexp(windowed[i],-first_exp-2,exponent_fault)),0);
    fft_engine(fft_input,fft_output,false);
    for(int i=0;i<1025;++i) {
        const double re=exact_ldexp(unpack_real(fft_output[i]),first_exp+2,exponent_fault);
        const double im=exact_ldexp(unpack_imag(fft_output[i]),first_exp+2,exponent_fault);
        const double p=re*re+im*im;
        const double floored=p>1e-12?p:1e-12;
        power[i%2][i/2]=floored;
        if(!noise_initialized) noise[i%2][i/2]=floored;
    }
    noise_initialized=true;
    unsigned current_fault;
    apply_three(power,noise,gamma,gain,alpha,beta,min_gain,current_fault);
    fault_state|=current_fault;
    maximum=0;
    for(int i=0;i<1025;++i) {
        const double re=exact_ldexp(unpack_real(fft_output[i]),first_exp+2,exponent_fault)*gain[i%2][i/2];
        const double im=exact_ldexp(unpack_imag(fft_output[i]),first_exp+2,exponent_fault)*gain[i%2][i/2];
        reduced_real[i]=re;reduced_imag[i]=im;
        if(std::fabs(re)>maximum) maximum=std::fabs(re);
        if(std::fabs(im)>maximum) maximum=std::fabs(im);
    }
    const int second_exp=exponent_of(maximum);
    // Inverse transform is unscaled. Restore block exponent and divide by N.
    for(int i=0;i<1025;++i) {
        const fft_input_t re=quantize_input(exact_ldexp(reduced_real[i],-second_exp-2,exponent_fault));
        const fft_input_t im=quantize_input(exact_ldexp(reduced_imag[i],-second_exp-2,exponent_fault));
        fft_input[i]=pack_input(re,im);
        if(i>0 && i<1024) fft_input[N-i]=pack_input(re,-im);
    }
    fft_engine(fft_input,fft_output,true);
    for(int i=0;i<HOP;++i) {
        const double w0=HANN[i],w1=HANN[i+HOP];
        const double current=exact_ldexp(unpack_real(fft_output[i]),second_exp+2-11,exponent_fault)*w0;
        const double overlap=tail[i]+current;
        const double norm=have_pending?w1*w1+w0*w0:w0*w0;
        double value=norm>1e-12?overlap/norm:0;
        if(!std::isfinite(value)) value=0;
        if(value>1)value=1;if(value< -1)value=-1;
        pending[i]=static_cast<float>(value);
        tail[i]=exact_ldexp(unpack_real(fft_output[i+HOP]),second_exp+2-11,exponent_fault)*w1;
    }
    have_pending=true;
    if(exponent_fault)fault_state|=2;
    numeric_fault=fault_state;
}
