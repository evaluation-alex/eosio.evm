// rust-evm.
// Copyright 2020 Stewart Mackenzie, Wei Tang, Matt Brubeck, Isaac Ardis, Elaine Ou.
// Licensed under the Apache License, Version 2.0.
// https://github.com/sorpaas/rust-evm/blob/dcd6e79a8a8ddaf955efe58131c6835e3aa59cfc/precompiled/modexp/src/lib.rs
// e-wasm
// Copyright 2020 e-wasm team
// Licensed under the Apache License, Version 2.0.
// https://github.com/ewasm/ewasm-precompiles/blob/master/modexp/src/lib.rs

#define BOOST_NO_STRINGSTREAM
#define BOOST_EXCEPTION_DISABLE 1

#include <eosio.evm/eosio.evm.hpp>
#include <boost/multiprecision/cpp_int.hpp>

namespace eosio_evm
{
  uint256_t Processor::mult_complexity(const uint256_t& len) {
    uint256_t term1 = len * len;
    uint256_t term2 = 0;
    uint256_t term3 = 0;

    if (len > 1024) {
      term1 /= 16;
      term2 = 46080;
      term3 = 199680;
    } else if (len > 64) {
      term1 /= 4;
      term2 = len * 96;
      term3 = 3072;
    }

    return term1 + term2 - term3;
  }

  uint256_t Processor::adjusted_exponent_length(uint256_t exponent_length, uint256_t base_length) {
    // Bound
    auto el = static_cast<uint64_t>(exponent_length);
    auto bl = static_cast<uint64_t>(base_length);

    // Params
    bool oversize  = el > 32;
    auto input_el  = oversize ? 32 : el;
    auto max_el    = oversize ? 0 : 32 - el;
    auto adjust_el = oversize ? (8 * el) - 256 : 0;

    // Right pad input with 0s
    auto padding = 8 * std::max(0ull, max_el);
    auto e = read_input(bl, input_el) >> padding;

    // MSB
    auto i = e ? -1 : 0;
    for (; e; e >>= 1, ++i);

    // MSB + Adjustment
    return i + adjust_el;
  }

  uint256_t Processor::read_input(uint64_t offset, uint64_t length) {
    // Read past end
    if (offset >= ctx->input.size()) {
      return {};
    }

    uint8_t data[32] = {};
    auto end = (offset + length) > ctx->input.size() ? ctx->input.size() : offset + length;
    auto data_offset = (end - offset) < 32 ? 32 - (end - offset) : 0;
    std::copy(std::begin(ctx->input) + offset, std::begin(ctx->input) + end, std::begin(data) + data_offset );

    // Debug
    // eosio::print("\n\n------read_input--------");
    // eosio::print("\n INPUT SIZE: ", ctx->input.size());
    // eosio::print("\n LENGTH: ", length);
    // eosio::print("\n START: ", offset);
    // eosio::print("\n END: ", end);
    // std::vector<uint8_t> test = {data, data + 32};
    // eosio::print("\nTest Hex: ", bin2hex(test));

    auto padding = (length - (end - offset)) * 8;
    uint256_t result = intx::be::load<uint256_t>(data) << padding;

    return result;
  }

  using bmi = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<>>;
  inline void bmi_to_bytes(bmi bigi, std::vector<uint8_t>& bytes) { auto byte_length = bytes.size(); while (byte_length != 0) { bytes[byte_length - 1] = static_cast<uint8_t>(0xff & bigi); bigi >>= 8; byte_length--; } }
  inline bmi bytes_to_bmi(const std::vector<uint8_t>& bytes) { bmi num = 0; for (auto byte: bytes) { num = num << 8 | byte; } return num; }

  bmi read_input_large(uint64_t offset, uint64_t length, Context* ctx) {
    // Read past end
    if (offset >= ctx->input.size()) {
      return {};
    }

    // Subset params
    auto end = (offset + length) > ctx->input.size() ? ctx->input.size() : offset + length;
    auto data_offset = (end - offset) < length ? length - (end - offset) : 0;

    // Subset memory
    std::vector<uint8_t> data(length);
    std::copy(std::begin(ctx->input) + offset, std::begin(ctx->input) + end, std::begin(data) + data_offset );

    // Create result
    auto padding = (length - (end - offset)) * 8;
    bmi num = bmi(bytes_to_bmi(data)) << padding;

    return num;
  }

  void Processor::precompile_expmod()
  {
    // Get b,e,m lengths
    uint256_t blen = read_input(0, 32);
    uint256_t elen = read_input(32, 32);
    uint256_t mlen = read_input(64, 32);

    // Gas param - complexity
    auto max = blen > mlen ? blen : mlen;
    auto complexity = mult_complexity(max);

    // Gas param - adjusted length
    auto adjusted = adjusted_exponent_length(elen, blen + 96);
    if (adjusted < 1) adjusted = 1;

    // Debug
    // eosio::print("\nInput: ", bin2hex(ctx->input));
    // eosio::print("\nB Len: ", intx::to_string(blen));
    // eosio::print("\nE Len: ", intx::to_string(elen));
    // eosio::print("\nM Len: ", intx::to_string(mlen));
    // eosio::print("\nComplexity: ", intx::to_string(complexity));
    // eosio::print("\nAdjusted: ", intx::to_string(adjusted));

    // Charge gas
    auto gas_cost = (adjusted * complexity) / GP_MODEXP;
    bool error = use_gas(gas_cost);
    if (error) return;

    if ((blen == 0 && mlen == 0) || mlen == 0) {
      return precompile_return({});
    }

    // Bounded lengths
    uint64_t blen2 = static_cast<uint64_t>(blen);
    uint64_t elen2 = static_cast<uint64_t>(elen);
    uint64_t mlen2 = static_cast<uint64_t>(mlen);

    // Get b,e,m
    auto b = read_input_large(96, blen2, ctx);
    auto e = read_input_large(96 + blen2, elen2, ctx);
    auto m = read_input_large(96 + blen2 + elen2, mlen2, ctx);

    // Debug
    std::vector<uint8_t> b_vec(static_cast<uint64_t>(blen));
    std::vector<uint8_t> e_vec(static_cast<uint64_t>(elen));
    std::vector<uint8_t> m_vec(static_cast<uint64_t>(mlen));
    bmi_to_bytes(b, b_vec);
    bmi_to_bytes(e, e_vec);
    bmi_to_bytes(m, m_vec);

    // Debug
    // eosio::print("\nB ", bin2hex(b_vec));
    // eosio::print("\nE ", bin2hex(e_vec));
    // eosio::print("\nM ", bin2hex(m_vec));

    // Empty vector of mod length
    std::vector<uint8_t> vec(static_cast<uint64_t>(mlen));

    // Mod less than equal 1
    if (m <= 1) {
      return precompile_return(vec);
    }

    // Execute
    bmi_to_bytes(boost::multiprecision::powm(b, e, m), vec);

    // Return the result.
    precompile_return(vec);
  }
} // namespace eosio_evm