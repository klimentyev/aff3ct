#include <sstream>

#include "Tools/Exception/exception.hpp"
#include "Tools/Display/Frame_trace/Frame_trace.hpp"

#include "Tools/Factory/Polar/Factory_frozenbits_generator.hpp"
#include "Tools/Factory/Polar/Factory_puncturer_polar.hpp"
#include "Tools/Factory/Polar/Factory_encoder_polar.hpp"
#include "Tools/Factory/Polar/Factory_decoder_polar_gen.hpp"

#include "Module/Puncturer/Polar/Puncturer_polar_wangliu.hpp"

#include "Codec_polar.hpp"

using namespace aff3ct::module;
using namespace aff3ct::tools;

template <typename B, typename Q>
Codec_polar<B,Q>
::Codec_polar(const typename Factory_encoder_common<B  >::encoder_parameters       &enc_params,
              const typename Factory_decoder_polar <B,Q>::decoder_parameters_polar &dec_params,
              const typename Factory_puncturer_polar<B,Q>::puncturer_parameters     &pct_params,
              const int n_threads)
: Codec_SISO<B,Q>(enc_params, dec_params), dec_par(dec_params), pct_par(pct_params),
  frozen_bits(dec_params.N),
  is_generated_decoder((dec_params.implem.find("_SNR") != std::string::npos)),
  fb_generator(nullptr),
  decoder_siso(n_threads, nullptr)
{
	if (!is_generated_decoder)
	{
		// build the frozen bits generator
		fb_generator = Factory_frozenbits_generator<B>::build(dec_par.fb_gen_method,
		                                                      dec_par.K,
		                                                      dec_par.N,
		                                                      dec_par.sigma,
		                                                      dec_par.awgn_fb_path,
		                                                      dec_par.bin_pb_path);

		if (fb_generator == nullptr)
			throw runtime_error(__FILE__, __LINE__, __func__, "'fb_generator' can't be null.");
	}
	else
	{
		if (dec_par.N != dec_par.N_pct)
		{
			std::stringstream message;
			message << "'N' has to be equal to 'N_pct' ('N' = " << dec_par.N
			        << ", 'N_pct' = " << dec_par.N_pct << ").";
			throw invalid_argument(__FILE__, __LINE__, __func__, message.str());
		}
	}
}

template <typename B, typename Q>
Codec_polar<B,Q>
::~Codec_polar()
{
	if (fb_generator != nullptr) { delete fb_generator; fb_generator = nullptr; }
}

template <typename B, typename Q>
void Codec_polar<B,Q>
::launch_precompute()
{
	if (!is_generated_decoder)
	{
		if (dec_par.sigma != 0.f)
		{
			fb_generator->generate(frozen_bits);
			if (dec_par.N != dec_par.N_pct)
			{
				Puncturer_polar_wangliu<B,Q> punct(dec_par.K, dec_par.N_pct, *fb_generator);
				punct.gen_frozen_bits(frozen_bits);
			}
		}
	}
	else
		Factory_decoder_polar_gen<B,Q>::get_frozen_bits(dec_par.implem,
		                                                dec_par.N,
		                                                frozen_bits);
}

template <typename B, typename Q>
void Codec_polar<B,Q>
::snr_precompute(const float sigma)
{
	// adaptative frozen bits generation
	if (dec_par.sigma == 0.f && !is_generated_decoder)
	{
		fb_generator->set_sigma(sigma);
		fb_generator->generate(frozen_bits);

		if (dec_par.N != dec_par.N_pct)
		{
			Puncturer_polar_wangliu<B,Q> punct(dec_par.K, dec_par.N_pct, *fb_generator);
			punct.gen_frozen_bits(frozen_bits);
		}
	}

//	if (this->params.simulation.debug)
//	{
//		std::clog << std::endl << "Frozen bits:" << std::endl;
//		Frame_trace<B> ft(this->params.simulation.debug_limit);
//		ft.display_bit_vector(frozen_bits);
//		std::clog << std::endl;
//	}
}

template <typename B, typename Q>
Puncturer<B,Q>* Codec_polar<B,Q>
::build_puncturer(const int tid)
{
	if(fb_generator == nullptr)
		throw runtime_error(__FILE__, __LINE__, __func__, "Polar puncturer requires a frozen bits generator.");

	return Factory_puncturer_polar<B,Q>::build(pct_par, *fb_generator);
}

template <typename B, typename Q>
Encoder<B>* Codec_polar<B,Q>
::build_encoder(const int tid, const Interleaver<int>* itl)
{
	return Factory_encoder_polar<B>::build(this->enc_params, frozen_bits);
}

template <typename B, typename Q>
SISO<Q>* Codec_polar<B,Q>
::build_siso(const int tid, const Interleaver<int>* itl, CRC<B>* crc)
{
	decoder_siso[tid] = Factory_decoder_polar<B,Q>::build_siso(dec_par, frozen_bits);
	return decoder_siso[tid];
}

template <typename B, typename Q>
Decoder<B,Q>* Codec_polar<B,Q>
::build_decoder(const int tid, const Interleaver<int>* itl, CRC<B>* crc)
{
	if (decoder_siso[tid] != nullptr)
	{
		auto ptr = decoder_siso[tid];
		decoder_siso[tid] = nullptr;
		return ptr;
	}
	else
	{
		if (is_generated_decoder)
			return Factory_decoder_polar_gen<B,Q>::build(dec_par, frozen_bits, this->enc_params.systematic, crc);
		else
			return Factory_decoder_polar    <B,Q>::build(dec_par, frozen_bits, this->enc_params.systematic, crc);
	}
}

template <typename B, typename Q>
void Codec_polar<B,Q>
::extract_sys_par(const mipp::vector<Q> &Y_N, mipp::vector<Q> &sys, mipp::vector<Q> &par)
{
	const auto K = dec_par.K;
	const auto N = dec_par.N;

	if ((int)Y_N.size() != N * dec_par.n_frames)
	{
		std::stringstream message;
		message << "'Y_N.size()' has to be equal to 'N' * 'inter_frame_level' ('Y_N.size()' = " << Y_N.size()
		        << ", 'N' = " << N << ", 'inter_frame_level' = " << dec_par.n_frames << ").";
		throw length_error(__FILE__, __LINE__, __func__, message.str());
	}

	if ((int)sys.size() != K * dec_par.n_frames)
	{
		std::stringstream message;
		message << "'sys.size()' has to be equal to 'K' * 'inter_frame_level' ('sys.size()' = " << sys.size()
		        << ", 'K' = " << K << ", 'inter_frame_level' = " << dec_par.n_frames << ").";
		throw length_error(__FILE__, __LINE__, __func__, message.str());
	}

	if ((int)par.size() != (N - K) * dec_par.n_frames)
	{
		std::stringstream message;
		message << "'par.size()' has to be equal to ('N' - 'K') * 'inter_frame_level' ('par.size()' = " << par.size()
		        << ", 'N' = " << N << ", 'K' = " << K << ", 'inter_frame_level' = "
		        << dec_par.n_frames << ").";
		throw length_error(__FILE__, __LINE__, __func__, message.str());
	}

	// extract systematic and parity information
	auto par_idx = 0, sys_idx = 0;
	for (auto f = 0; f < dec_par.n_frames; f++)
		for (auto i = 0; i < N; i++)
			if (!frozen_bits[i]) // add La on information (systematic) bits
			{
				sys[sys_idx] = Y_N[f * N +i];
				sys_idx++;
			}
			else // parity bit
			{
				par[par_idx] = Y_N[f * N +i];
				par_idx++;
			}
}

// ==================================================================================== explicit template instantiation 
#include "Tools/types.h"
#ifdef MULTI_PREC
template class aff3ct::tools::Codec_polar<B_8,Q_8>;
template class aff3ct::tools::Codec_polar<B_16,Q_16>;
template class aff3ct::tools::Codec_polar<B_32,Q_32>;
template class aff3ct::tools::Codec_polar<B_64,Q_64>;
#else
template class aff3ct::tools::Codec_polar<B,Q>;
#endif
// ==================================================================================== explicit template instantiation
