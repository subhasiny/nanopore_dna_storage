#include <omp.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include "cxxopts.hpp"

const uint8_t NBASE = 4;
const char int2base[NBASE] = {'A', 'C', 'G', 'T'};
const uint8_t nstate_crf = 8;
// states are A+,C+,G+,T+,A-,C-,G-,T- (+ = flip, - = flop), you can enter from a
// different base only into a flip base.

typedef std::array<std::array<float, nstate_crf>, NBASE + 1> crf_mat_t;
// for each input state, we have five (4+1) possible output states: e.g., A+ ->
// A+,C+,G+,T+,A-; A- -> A+,C+,G+,T+,A-; A-; etc.

// for parallel LVA
const uint32_t BITSET_SIZE = 256;  // 32 bytes
typedef std::bitset<BITSET_SIZE> bitset_t;
struct LVA_path_t {
  bitset_t msg;
  float score;
};

// struct for storing information about previous state for a current state and
// the transition
struct prev_state_info_t {
  uint32_t st_conv;
  uint8_t st_crf;
  uint8_t post_idx_0;
  uint8_t
      post_idx_1;     // the transition score is in post[post_idx_0][post_idx_1]
  uint8_t msg_shift;  // shift in message in transition
  uint8_t msg_newbits;  // new bits in transition (new_msg = (old_msg <<
                        // msg_shift)|msg_newbits)
};

// heap element used for finding the top list_size elements
struct heap_elem_t {
  float score;
  uint32_t pos_in_prev_state;
  uint32_t pos_in_list;
  uint32_t prev_state;
  bool operator<(const heap_elem_t &h) { return (score < h.score); }
};

bool rc_flag = false;

// convolutional code related parameters
// (set in set_conv_params())

uint8_t mem_conv;
uint32_t nstate_conv;
const uint8_t n_out_conv = 2;
uint32_t G[n_out_conv];  // octal
uint32_t initial_state_conv;
uint32_t final_state_conv;
uint32_t sync_marker_length;
bool sync_marker[BITSET_SIZE];
uint32_t sync_marker_period;
const uint8_t MAX_PUNCTURING_PATTERN_SIZE = 16;
uint8_t puncturing_pattern[MAX_PUNCTURING_PATTERN_SIZE];
uint8_t puncturing_pattern_len;
uint32_t nstate_pos;
uint32_t st_pos2msg_pos[BITSET_SIZE];  // map st_pos to position in message

int set_conv_params(uint8_t mem_conv_param, uint8_t rate_param,
                    uint32_t msg_len_param,
                    const std::string sync_marker_param = "",
                    const uint32_t sync_marker_period_param = 0);

std::vector<bool> conv_encode(const std::vector<bool> &msg);

std::vector<std::vector<bool>> read_bit_array(const std::string &infile);

void write_bit_array(const std::vector<bool> &outvec,
                     const std::string &outfile);

void write_bit_array_in_bases(const std::vector<std::vector<bool>> &outvec_vec,
                              const std::string &outfile);

template <class T>
void write_vector(const std::vector<T> &outvec, const std::string &outfile) {
  // write values in vector, one per line
  std::ofstream fout(outfile);
  for (auto v : outvec) {
    fout << v << "\n";
  }
  fout.close();
}

float logsumexpf(float x, float y) {
  float max_x_y = std::max(x, y);
  return max_x_y + logf(expf(x - max_x_y) + expf(y - max_x_y));
}

std::vector<crf_mat_t> read_crf_post(const std::string &infile);

std::vector<std::vector<uint8_t>> read_vocab_file(const std::string &infile);

uint8_t to_idx_crf_in_post(uint8_t st2_crf);

uint32_t get_state_idx(const uint32_t st_pos, const uint32_t st_conv,
                       const uint32_t st_crf);

uint32_t conv_next_state(const uint32_t cur_state, const bool bit);

uint32_t conv_prev_state(const uint32_t cur_state, const bool bit);

uint32_t conv_output(const uint8_t output_idx, const uint32_t cur_state,
                     const bool bit);

bool is_valid_state(const uint32_t &st2_pos, const uint32_t &st2_conv,
                    const uint32_t &msg_len);

std::vector<std::vector<bool>> decode_post_conv_parallel_LVA(
    const std::vector<crf_mat_t> &post, const uint32_t msg_len,
    const uint32_t list_size, const uint32_t num_thr,
    const uint32_t max_deviation);

std::vector<prev_state_info_t> find_prev_states(const uint32_t &st2_conv,
                                                const uint32_t &st2_crf,
                                                const uint8_t &punc_pattern);

uint32_t reverse_integer_bits(const uint32_t &num, const uint32_t numbits);

int main(int argc, char **argv) {
  cxxopts::Options options("viterbi_nanopore",
                           "Viterbi decoder for nanopore dna storage codes");
  options.add_options()("m,mode", "Mode: encode, decode",
                        cxxopts::value<std::string>())(
      "i,infile",
      "Infile with message (encoding) or posterior matrix (decoding)",
      cxxopts::value<std::string>())(
      "o,outfile", "Outfile with encoded/decoded message (list)",
      cxxopts::value<std::string>())("msg-len", "Message length",
                                     cxxopts::value<uint32_t>())(
      "mem-conv", "Code memory for convolutional code",
      cxxopts::value<uint8_t>())(
      "sync-marker",
      "Sync marker for convolutional code decoding as string (e.g. 110) "
      "(default '')",
      cxxopts::value<std::string>()->default_value(""))(
      "sync-period", "Sync marker period for convolutional code decoding",
      cxxopts::value<uint32_t>())(
      "l,list-size", "List size for convolutional code decoding (default 1)",
      cxxopts::value<uint32_t>()->default_value("1"))(
      "r,rate",
      "Rate of convolutional code: options 1 (1/2), 2 (2/3), 3 (3/4), 4 (4/5), "
      "5 (5/6), 7 (7/8) "
      "(default 1). Use standard puncturing patterns, expects appropriate "
      "padding (at most 1 bit needed) to make output length even.",
      cxxopts::value<uint8_t>()->default_value("1"))(
      "max-deviation",
      "Max allowable deviation of st_pos around its expected value during "
      "decoding (tradeoff b/w speed and accuracy) (default: infinite)",
      cxxopts::value<uint32_t>())("rc",
                                  "Reverse complement read (for decoding)")(
      "t,num-thr",
      "Number of threads for convolutional code decoding (default 1)",
      cxxopts::value<uint32_t>()->default_value("1"))("h,help",
                                                      "Display this message");
  auto result = options.parse(argc, argv);

  if (result.count("help")) {
    std::cout << options.help() << "\n";
    return 0;
  }
  if (!result.count("mode") || !result.count("infile") ||
      !result.count("outfile")) {
    std::cout << "Invalid options.\n";
    std::cout << options.help() << "\n";
    return -1;
  }
  std::string mode = result["mode"].as<std::string>();
  std::string infile = result["infile"].as<std::string>();
  std::string outfile = result["outfile"].as<std::string>();
  if (mode == "encode" || mode == "decode") {
    if (!result.count("mem-conv")) {
      std::cout << "Memory of convolutional code not specified.\n";
      std::cout << options.help() << "\n";
      return -1;
    }
    if (!result.count("msg-len")) {
      std::cout << "msg-len not specified.\n";
      std::cout << options.help() << "\n";
      return -1;
    }
    uint32_t msg_len = result["msg-len"].as<uint32_t>();
    int status = 0;
    if (mode == "decode") rc_flag = result["rc"].as<bool>();
    if (rc_flag) std::cout << "Reverse complement flag detected.\n";
    std::string sync_marker_param = result["sync-marker"].as<std::string>();
    if (sync_marker_param == "")
      status = set_conv_params(result["mem-conv"].as<uint8_t>(),
                               result["rate"].as<uint8_t>(), msg_len);
    else
      status = set_conv_params(
          result["mem-conv"].as<uint8_t>(), result["rate"].as<uint8_t>(),
          msg_len, sync_marker_param, result["sync-period"].as<uint32_t>());
    if (status != 0) {
      std::cout << options.help() << "\n";
      return -1;
    }
    if (mode == "encode") {
      std::vector<std::vector<bool>> msg_vec = read_bit_array(infile);
      std::vector<std::vector<bool>> encoded_msg_vec;
      for (auto msg : msg_vec) {
        if (msg.size() != msg_len) {
          std::cout << "Message length does not match msg_len parameter.\n";
          return -1;
        }
        encoded_msg_vec.push_back(conv_encode(msg));
      }
      write_bit_array_in_bases(encoded_msg_vec, outfile);
    } else {
      // do list decoding
      //
      // find list_size top paths using Parallel LVA as described in
      // https://github.com/shubhamchandak94/kBestViterbi/blob/master/kBestViterbi.py
      // or in ieeexplore.ieee.org/iel1/26/12514/00577040.pdf - if you get two
      // paths coming to same state at same time with same message, then keep
      // only one and take the max score - thus we try to get list_size unique
      // msg at each stage (if we don't do this, we observed that most of the
      // paths at the end correspond to the same msg)
      //

      uint32_t max_deviation =
          msg_len + mem_conv +
          1;  // don't restrict anything, do full exact Viterbi
      if (result.count("max-deviation"))
        max_deviation = result["max-deviation"].as<uint32_t>();
      std::vector<crf_mat_t> post = read_crf_post(infile);
      uint32_t list_size = result["list-size"].as<uint32_t>();
      uint32_t num_thr = result["num-thr"].as<uint32_t>();
      auto decoded_msg_list = decode_post_conv_parallel_LVA(
          post, msg_len, list_size, num_thr, max_deviation);
      std::ofstream fout(outfile);
      for (auto decoded_msg : decoded_msg_list) {
        for (auto decoded_msg_bit : decoded_msg)
          fout << std::to_string(decoded_msg_bit);
        fout << "\n";
      }
    }
  } else {
    std::cout << "Invalid mode.\n";
    std::cout << options.help() << "\n";
    return -1;
  }

  return 0;
}

int set_conv_params(uint8_t mem_conv_param, uint8_t rate_param,
                    uint32_t msg_len_param, const std::string sync_marker_param,
                    const uint32_t sync_marker_period_param) {
  mem_conv = mem_conv_param;
  nstate_conv = 1 << mem_conv;
  switch (mem_conv) {
    case 6:
      G[0] = 0171;
      G[1] = 0133;
      initial_state_conv = 0b100101;
      break;
    case 8:
      G[0] = 0515;
      G[1] = 0677;
      initial_state_conv = 0b10010110;
      break;
    case 11:
      G[0] = 05537;
      G[1] = 06131;
      initial_state_conv = 0b10010110001;
      break;
    case 14:
      G[0] = 075063;
      G[1] = 056711;
      initial_state_conv = 0b10010110001101;
      break;
    default:
      std::cout << "Invalid mem_conv (allowed: 6, 8, 11, 14)\n";
      return -1;
  }
  final_state_conv = reverse_integer_bits(initial_state_conv, mem_conv);

  // puncturing pattern representing using an array: 0 representing 1\n1, 1
  // representing 0,1\n1,0, 2 representing 1,0\n0,1, 3 representing 0,0\n1,1
  // (these are the only patterns supported right now)
  switch (rate_param) {
    case 1:
      puncturing_pattern[0] = 0;
      puncturing_pattern_len = 1;
      break;
    case 2:
      puncturing_pattern[0] = 0;
      puncturing_pattern[1] = 2;
      puncturing_pattern[2] = 0;
      puncturing_pattern_len = 3;
      break;
    case 3:
      puncturing_pattern[0] = 0;
      puncturing_pattern[1] = 1;
      puncturing_pattern_len = 2;
      break;
    case 4:
      puncturing_pattern[0] = 0;
      puncturing_pattern[1] = 3;
      puncturing_pattern[2] = 0;
      puncturing_pattern[3] = 2;
      puncturing_pattern[4] = 1;
      puncturing_pattern_len = 5;
      break;
    case 5:
      puncturing_pattern[0] = 0;
      puncturing_pattern[1] = 1;
      puncturing_pattern[2] = 2;
      puncturing_pattern_len = 3;
      break;
    case 7:
      puncturing_pattern[0] = 0;
      puncturing_pattern[1] = 3;
      puncturing_pattern[2] = 1;
      puncturing_pattern[3] = 1;
      puncturing_pattern_len = 4;
      break;
    default:
      std::cout << "Invalid rate parameter (allowed: 1, 3, 5, 7)\n";
      return -1;
  }
  // verify that msg_len is such that output length is even (for converting to
  // DNA). also set nstate_pos (which corresponds now to the output position
  // rather than input). also set st_pos2msg_pos (where the msg_pos is indexed
  // by 1 (so ranges from 1 to msg_len_param + mem_conv)).
  nstate_pos = 1;  // state 0 (init)
  st_pos2msg_pos[0] = 0;
  uint32_t i = 0;
  while (i < msg_len_param + mem_conv) {
    i += ((puncturing_pattern[(nstate_pos - 1) % puncturing_pattern_len] == 0)
              ? 1
              : 2);
    st_pos2msg_pos[nstate_pos++] = i;
  }
  if (i != msg_len_param + mem_conv) {
    std::cout
        << "Output length not even. Try padding with a single 0 at end.\n";
    return -1;
  }

  if (rc_flag) {
    // basically reverse everything
    G[0] = reverse_integer_bits(G[0], mem_conv + 1);
    G[1] = reverse_integer_bits(G[1], mem_conv + 1);
    uint32_t temp = reverse_integer_bits(initial_state_conv, mem_conv);
    initial_state_conv = reverse_integer_bits(final_state_conv, mem_conv);
    final_state_conv = temp;

    // now do st_pos2msg_pos and puncturing_pattern
    uint32_t temp_puncturing_pattern[BITSET_SIZE];
    for (uint32_t i = 0; i < puncturing_pattern_len; i++)
      temp_puncturing_pattern[i] = puncturing_pattern[i];
    // first find where end of forward read lies in terms of puncturing pattern
    uint32_t punc_pattern_end_idx =
        (nstate_pos - 2) %
        puncturing_pattern_len;  // -2 because st_pos is 1 indexed
    uint8_t punc_pattern_reverse_map[4] = {
        0, 2, 1, 3};  // 1 <-> 2 when pattern looked in opposite direction
    for (uint32_t i = 0; i < puncturing_pattern_len; i++) {
      uint32_t j = (puncturing_pattern_len - i + punc_pattern_end_idx) %
                   puncturing_pattern_len;
      puncturing_pattern[i] =
          punc_pattern_reverse_map[temp_puncturing_pattern[j]];
    }
    std::reverse(st_pos2msg_pos, st_pos2msg_pos + nstate_pos);
    for (uint32_t i = 0; i < nstate_pos; i++)
      st_pos2msg_pos[i] = msg_len_param + mem_conv - st_pos2msg_pos[i];
  }

  if (sync_marker_param == "") return 0;

  sync_marker_length = sync_marker_param.length();
  if (sync_marker_length >= BITSET_SIZE) {
    std::cout << "Too large sync marker\n";
    return -1;
  }
  sync_marker_period = sync_marker_period_param;
  if (sync_marker_period < sync_marker_length) {
    std::cout << "Sync period shorter than sync marker length\n";
    return -1;
  }

  for (uint32_t i = 0; i < sync_marker_length; i++) {
    switch (sync_marker_param[i]) {
      case '0':
        sync_marker[i] = 0;
        break;
      case '1':
        sync_marker[i] = 1;
        break;
      default:
        std::cout << "Invalid character in sync marker (only 0/1 allowed)\n";
        return -1;
    }
  }
  return 0;
}

uint32_t reverse_integer_bits(const uint32_t &num, const uint32_t numbits) {
  uint32_t ret = 0;
  for (uint32_t i = 0; i < numbits; i++) {
    ret <<= 1;
    ret |= ((num >> i) & 1);
  }
  return ret;
}

uint32_t conv_next_state(const uint32_t cur_state, const bool bit) {
  if (bit)
    return ((cur_state | nstate_conv) >> 1);
  else
    return (cur_state >> 1);
}

uint32_t conv_prev_state(const uint32_t cur_state, const bool bit) {
  if (bit)
    return ((cur_state << 1) | 1) & (nstate_conv - 1);
  else
    return (cur_state << 1) & (nstate_conv - 1);
}

uint32_t conv_output(const uint8_t output_idx, const uint32_t cur_state,
                     const bool bit) {
  // XOR with rc_flag to capture reverse complementation of base
  if (bit)
    return (__builtin_parity(((cur_state | nstate_conv) & G[output_idx]))) ^
           (rc_flag);
  else
    return (__builtin_parity((cur_state & G[output_idx]))) ^ (rc_flag);
}

std::vector<bool> conv_encode(const std::vector<bool> &msg) {
  std::vector<bool> encoded_msg;
  uint32_t cur_state = initial_state_conv;
  for (bool msg_bit : msg) {
    encoded_msg.push_back(conv_output(0, cur_state, msg_bit));
    encoded_msg.push_back(conv_output(1, cur_state, msg_bit));
    cur_state = conv_next_state(cur_state, msg_bit);
  }
  // add terminating bits
  for (uint8_t i = 0; i < mem_conv; i++) {
    bool padding_bit = (final_state_conv >> i) & 1;
    encoded_msg.push_back(conv_output(0, cur_state, padding_bit));
    encoded_msg.push_back(conv_output(1, cur_state, padding_bit));
    cur_state = conv_next_state(cur_state, padding_bit);
  }
  if (cur_state != final_state_conv)
    throw std::runtime_error(
        "state after encoding does not match final_state_conv");
  std::vector<bool> encoded_msg_punctured;
  // puncture
  uint32_t i = 0;  // position in encoded message
  for (uint32_t st_pos = 0; st_pos < nstate_pos - 1; st_pos++) {
    uint8_t punc_pattern = puncturing_pattern[st_pos % puncturing_pattern_len];
    switch (punc_pattern) {
      case 0:
        encoded_msg_punctured.push_back(encoded_msg[i]);
        encoded_msg_punctured.push_back(encoded_msg[i + 1]);
        i += 2;
        break;
      case 1:
        encoded_msg_punctured.push_back(encoded_msg[i + 1]);
        encoded_msg_punctured.push_back(encoded_msg[i + 2]);
        i += 4;
        break;
      case 2:
        encoded_msg_punctured.push_back(encoded_msg[i]);
        encoded_msg_punctured.push_back(encoded_msg[i + 3]);
        i += 4;
        break;
      case 3:
        encoded_msg_punctured.push_back(encoded_msg[i + 1]);
        encoded_msg_punctured.push_back(encoded_msg[i + 3]);
        i += 4;
        break;
    }
  }
  if (i != encoded_msg.size())
    throw std::runtime_error("error in encoding, lengths don't match");
  return encoded_msg_punctured;
}

std::vector<std::vector<bool>> read_bit_array(const std::string &infile) {
  std::ifstream fin(infile);
  std::vector<std::vector<bool>> vec_vec;
  std::vector<bool> vec;
  char ch;
  while (fin >> std::noskipws >> ch) {
    switch (ch) {
      case '0':
        vec.push_back(0);
        break;
      case '1':
        vec.push_back(1);
        break;
      case '\n':
        vec_vec.push_back(vec);
        vec.clear();
        break;
      default:
        throw std::runtime_error("invalid character in input file");
    }
  }
  fin.close();
  return vec_vec;
}

void write_bit_array(const std::vector<bool> &outvec,
                     const std::string &outfile) {
  std::ofstream fout(outfile);
  for (bool b : outvec) fout << (b ? '1' : '0');
  fout.close();
}

void write_char_array(const std::vector<char> &vec,
                      const std::string &outfile) {
  std::ofstream fout(outfile);
  for (char c : vec) fout << c;
  fout.close();
}

void write_bit_array_in_bases(const std::vector<std::vector<bool>> &outvec_vec,
                              const std::string &outfile) {
  std::ofstream fout(outfile);
  for (auto outvec : outvec_vec) {
    uint32_t len = outvec.size();
    if (len % 2 != 0) throw std::runtime_error("length not even");
    for (uint32_t i = 0; i < len / 2; i++)
      fout << int2base[2 * outvec[2 * i] + outvec[2 * i + 1]];
    fout << "\n";
  }
  fout.close();
}

std::vector<crf_mat_t> read_crf_post(const std::string &infile) {
  std::ifstream fin(infile, std::ios::binary);
  std::vector<crf_mat_t> post;
  crf_mat_t post_mat;
  float val;
  fin.read((char *)&val, sizeof(float));
  while (!fin.eof()) {
    for (uint8_t i = 0; i < NBASE; i++) {
      for (uint8_t j = 0; j < nstate_crf; j++) {
        post_mat[i][j] = val;
        fin.read((char *)&val, sizeof(float));
      }
    }
    uint8_t i = NBASE;  // output state is one of the flop states now (for given
                        // input state, only one possible output flop state)
    for (uint8_t j = 0; j < nstate_crf; j++) {
      post_mat[i][j] = val;
      fin.read((char *)&val, sizeof(float));
    }
    post.push_back(post_mat);
  }
  return post;
}

uint32_t get_state_idx(const uint32_t st_pos, const uint32_t st_conv,
                       const uint32_t st_crf) {
  return st_pos * nstate_conv * nstate_crf + st_conv * nstate_crf + st_crf;
}

uint8_t to_idx_crf_in_post(uint8_t st2_crf) {
  // return index of the st2_crf state in the post matrix, we need this because
  // we have 5 by 8 matrix and transitions to flop states are stored in the last
  // row to save space since not all transitions to the flop state are allowed
  return (st2_crf >= NBASE) ? NBASE : st2_crf;
}

std::vector<std::vector<bool>> decode_post_conv_parallel_LVA(
    const std::vector<crf_mat_t> &post, const uint32_t msg_len,
    const uint32_t list_size, const uint32_t num_thr,
    const uint32_t max_deviation) {
  omp_set_num_threads(num_thr);
  float INF = std::numeric_limits<float>::infinity();
  uint64_t nstate_total_64 = nstate_pos * nstate_crf * nstate_conv;
  if (nstate_total_64 >= ((uint64_t)1 << 32))
    throw std::runtime_error("Too many states, can't fit in 32 bits");
  uint32_t nstate_total = (uint32_t)nstate_total_64;
  uint32_t nblk = post.size();
  if (post.size() < nstate_pos + 1)
    throw std::runtime_error("Too small post matrix");

  // instead of traceback, store the msg till now as a bitset
  if (msg_len > BITSET_SIZE)
    throw std::runtime_error("msg_len can't be above BITSET_SIZE");

  bitset_t **curr_best_msg = new bitset_t *[nstate_total];
  bitset_t **prev_best_msg = new bitset_t *[nstate_total];
  float **curr_score = new float *[nstate_total];
  float **prev_score = new float *[nstate_total];
  for (uint32_t i = 0; i < nstate_total; i++) {
    curr_best_msg[i] = new bitset_t[list_size]();
    prev_best_msg[i] = new bitset_t[list_size]();
    curr_score[i] = new float[list_size]();
    prev_score[i] = new float[list_size]();
    for (uint32_t j = 0; j < list_size; j++) {
      curr_score[i][j] = -INF;
      prev_score[i][j] = -INF;
    }
  }

  // find valid states based on intial and final states as well as
  // synchronization markers
  std::vector<bool> valid_state_array(nstate_pos * nstate_conv);
#pragma omp parallel
#pragma omp for
  for (uint32_t st_pos = 0; st_pos < nstate_pos; st_pos++)
    for (uint32_t st_conv = 0; st_conv < nstate_conv; st_conv++)
      valid_state_array[nstate_conv * st_pos + st_conv] =
          is_valid_state(st_pos2msg_pos[st_pos], st_conv, msg_len);

  // precompute the previous states and associated info for all states now
  // note that this is valid only for st_pos > 0 (if st_pos = 0, only previous
  // state allowed is same state - which is always first entry in the
  // prev_state_vector)
  std::vector<std::vector<std::vector<prev_state_info_t>>> prev_state_vector(4);
#pragma omp parallel
#pragma omp for
  for (uint8_t punc_pattern = 0; punc_pattern < 4; punc_pattern++) {
    // only fill if this punc_pattern is relevant for this rate
    if (std::find(puncturing_pattern,
                  puncturing_pattern + puncturing_pattern_len,
                  punc_pattern) == puncturing_pattern + puncturing_pattern_len)
      continue;
    prev_state_vector[punc_pattern].resize(nstate_conv * nstate_crf);
    for (uint32_t st_conv = 0; st_conv < nstate_conv; st_conv++)
      for (uint8_t st_crf = 0; st_crf < nstate_crf; st_crf++)
        prev_state_vector[punc_pattern][nstate_crf * st_conv + st_crf] =
            find_prev_states(st_conv, st_crf, punc_pattern);
  }

  // unordered_sets used for finding duplicate messages while building list
  //  std::vector<std::unordered_set<bitset_t>> already_seen_set(num_thr);
  // NOT USING SET SINCE IT'S SLOWER THAN JUST SEARCHING THROUGH THE LIST FOR
  // SMALL LIST SIZES

  for (uint8_t st_crf = 0; st_crf < nstate_crf; st_crf++) {
    curr_score[get_state_idx(0, initial_state_conv, st_crf)][0] =
        0.0;  // only valid initial state is pos 0, conv code at
              // initial_state_conv. crf state can be anything since we will
              // later ignore everything before the first transition
              // only populate one position in list (0), rest -INF
  }

  // forward Viterbi pass

  for (uint32_t t = 0; t < nblk; t++) {
    // swap prev and curr arrays
    std::swap(curr_score, prev_score);
    std::swap(curr_best_msg, prev_best_msg);

    // st is current state
    uint32_t st_pos_start =
        std::max((int64_t)nstate_pos - 2 - (nblk - 1 - t), (int64_t)0);
    uint32_t st_pos_end = std::min(t + 2, nstate_pos);

    st_pos_start = std::max(
        (int64_t)0, (int64_t)((double)(t) / nblk * nstate_pos - max_deviation));
    st_pos_end = std::min(st_pos_start + 2 * max_deviation, nstate_pos);

    // only allow pos which can have non -INF scores or will lead to useful
    // final states initially large pos is not allowed, and at the end small
    // pos not allowed (since those can't lead to correct st_pos at the end).

#pragma omp parallel
#pragma omp for schedule(dynamic)
    for (uint32_t st_pos = st_pos_start; st_pos < st_pos_end; st_pos++) {
      // heaps used for finding top L list
      // each element in heap just contains the score, the index in
      // prev_state_vector and the position in the list
      std::vector<heap_elem_t> heap;

      uint8_t punc_pattern = 0;
      if (st_pos != 0)
        punc_pattern =
            puncturing_pattern[(st_pos - 1) % puncturing_pattern_len];

      for (uint32_t st_conv = 0; st_conv < nstate_conv; st_conv++) {
        // check if this is a valid state, otherwise continue
        if (!valid_state_array[nstate_conv * st_pos + st_conv]) continue;
        for (uint8_t st_crf = 0; st_crf < nstate_crf; st_crf++) {
          uint32_t st = get_state_idx(st_pos, st_conv, st_crf);

          const auto &prev_states_st =
              prev_state_vector[punc_pattern][nstate_crf * st_conv + st_crf];
          if (st_pos == 0) {
            // only allowed previous state is st (which is 0th index in
            // prev_states)
            curr_best_msg[st][0] = prev_best_msg[st][0];
            curr_score[st][0] = prev_score[st][0] +
                                post[t][prev_states_st[0].post_idx_0]
                                    [prev_states_st[0].post_idx_1];
            for (uint32_t l = 1; l < list_size; l++) curr_score[st][l] = -INF;
          } else {
            if (list_size == 1) {
              // special case, just find max score
              float best_score = -INF;
              uint32_t best_score_idx = 0;
              uint32_t best_prev_state = 0;
              for (uint32_t psidx = 0; psidx < prev_states_st.size(); psidx++) {
                uint32_t prev_st_pos = st_pos - ((psidx == 0) ? 0 : 1);
                uint32_t prev_state =
                    get_state_idx(prev_st_pos, prev_states_st[psidx].st_conv,
                                  prev_states_st[psidx].st_crf);
                float score = prev_score[prev_state][0] +
                              post[t][prev_states_st[psidx].post_idx_0]
                                  [prev_states_st[psidx].post_idx_1];
                if (score > best_score) {
                  best_score = score;
                  best_score_idx = psidx;
                  best_prev_state = prev_state;
                }
              }
              if (best_score == -INF) {
                curr_score[st][0] = -INF;
              } else {
                curr_score[st][0] = best_score;
                curr_best_msg[st][0] =
                    (prev_best_msg[best_prev_state][0]
                     << prev_states_st[best_score_idx].msg_shift) |
                    bitset_t(prev_states_st[best_score_idx].msg_newbits);
              }
            } else {
              // clear heap
              heap.clear();
              // clear set
              //              already_seen_set[tid].clear();

              // put things in heap
              for (uint32_t psidx = 0; psidx < prev_states_st.size(); psidx++) {
                uint32_t prev_st_pos = st_pos - ((psidx == 0) ? 0 : 1);
                uint32_t prev_state =
                    get_state_idx(prev_st_pos, prev_states_st[psidx].st_conv,
                                  prev_states_st[psidx].st_crf);
                if (prev_score[prev_state][0] != -INF) {
                  float score = prev_score[prev_state][0] +
                                post[t][prev_states_st[psidx].post_idx_0]
                                    [prev_states_st[psidx].post_idx_1];
                  heap.push_back({score, psidx, 0, prev_state});
                }
              }
              std::make_heap(heap.begin(), heap.end());
              uint32_t l = 0;  // position in list
              while (!heap.empty() && l < list_size) {
                // pop top element from heap
                std::pop_heap(heap.begin(), heap.end());
                auto h = heap.back();
                heap.pop_back();
                uint32_t psidx = h.pos_in_prev_state;
                uint32_t prev_state = h.prev_state;
                bitset_t msg = prev_best_msg[prev_state][h.pos_in_list];
                uint8_t msg_shift = prev_states_st[psidx].msg_shift;
                uint8_t msg_newbits = prev_states_st[psidx].msg_newbits;
                msg = (msg << msg_shift) | bitset_t(msg_newbits);
                // put in list if not already seen
                //                      if (already_seen_set[tid].count(msg) ==
                //                      0) {
                if (std::find(curr_best_msg[st], curr_best_msg[st] + l, msg) ==
                    curr_best_msg[st] + l) {
                  curr_best_msg[st][l] = msg;
                  curr_score[st][l] = h.score;
                  //                        already_seen_set[tid].insert(msg);
                  l++;
                }

                // push next element in the list in heap if score not -INF and
                // list not over
                if (h.pos_in_list == list_size - 1) continue;
                float score_wo_post = prev_score[prev_state][h.pos_in_list + 1];
                if (score_wo_post != -INF) {
                  float score = score_wo_post +
                                post[t][prev_states_st[psidx].post_idx_0]
                                    [prev_states_st[psidx].post_idx_1];
                  heap.push_back({score, psidx, h.pos_in_list + 1, prev_state});
                  std::push_heap(heap.begin(), heap.end());
                }
              }
              // fill any remaining positions in list with -INF
              for (; l < list_size; l++) curr_score[st][l] = -INF;
            }
          }
        }
      }
    }
  }
  std::vector<LVA_path_t> LVA_path_list_final;
  uint32_t st_pos = nstate_pos - 1, st_conv = final_state_conv;  // last state
  for (uint8_t st_crf = 0; st_crf < nstate_crf; st_crf++) {
    uint32_t st = get_state_idx(st_pos, st_conv, st_crf);
    for (uint32_t list_pos = 0; list_pos < list_size; list_pos++) {
      if (curr_score[st][list_pos] != -INF)
        LVA_path_list_final.push_back(
            {curr_best_msg[st][list_pos], curr_score[st][list_pos]});
    }
  }

  // sort
  std::sort(LVA_path_list_final.begin(), LVA_path_list_final.end(),
            [](const LVA_path_t &p1, const LVA_path_t &p2) -> bool {
              return p1.score > p2.score;
            });

  if (LVA_path_list_final.size() > list_size)
    LVA_path_list_final.resize(list_size);

  std::vector<std::vector<bool>> decoded_msg_list;

  // now convert bitset to bool vectors
  for (auto LVA_path : LVA_path_list_final) {
    std::vector<bool> decoded_msg(msg_len);
    for (uint8_t i = 0; i < msg_len; i++)
      decoded_msg[i] =
          LVA_path.msg[msg_len + mem_conv - 1 -
                       i];  // due to way bitset is stored in reverse
    if (rc_flag) std::reverse(decoded_msg.begin(), decoded_msg.end());
    decoded_msg_list.push_back(decoded_msg);

    // FOR DEBUGGING
    /*
        std::cout << "score: " << LVA_path.score << "\n";
        for (auto b : decoded_msg_list.back()) std::cout << b;
        std::cout << "\n\n";
    */
  }
  //  std::cout << "Final list size: " << decoded_msg_list.size() << "\n";

  for (uint32_t i = 0; i < nstate_total; i++) {
    delete[] curr_best_msg[i];
    delete[] prev_best_msg[i];
    delete[] curr_score[i];
    delete[] prev_score[i];
  }
  delete[] curr_best_msg;
  delete[] prev_best_msg;
  delete[] curr_score;
  delete[] prev_score;
  return decoded_msg_list;
}

std::vector<prev_state_info_t> find_prev_states(const uint32_t &st2_conv,
                                                const uint32_t &st2_crf,
                                                const uint8_t &punc_pattern) {
  std::vector<prev_state_info_t> prev_state_vec;
  prev_state_info_t prev_state_info;
  uint8_t curr_conv_bit = (st2_conv >> (mem_conv - 1));
  uint8_t curr_conv_bit_1 = (st2_conv >> (mem_conv - 2)) & 1;
  // first do stay
  uint32_t st1_conv = st2_conv;
  uint8_t st1_crf = st2_crf;
  prev_state_info.st_conv = st1_conv;
  prev_state_info.st_crf = st1_crf;
  prev_state_info.post_idx_0 = to_idx_crf_in_post(st2_crf);
  prev_state_info.post_idx_1 = st1_crf;
  prev_state_info.msg_shift = 0;
  prev_state_info.msg_newbits = 0;
  prev_state_vec.push_back(prev_state_info);

  for (uint8_t st1_crf = 0; st1_crf < nstate_crf; st1_crf++) {
    if (st2_crf >= NBASE &&
        !((st1_crf == st2_crf) || st1_crf == st2_crf - NBASE))
      continue;  // unallowed transition
    if (st2_crf == st1_crf) {
      // at same base, already done above
      continue;
    }
    // new base, new position
    // look at two possible previous states of convolutional code and
    // see if the output for the transition matches the base st2_crf
    uint8_t st2_crf_base = st2_crf % NBASE;
    if (punc_pattern == 0) {
      for (uint8_t conv_bit = 0; conv_bit < 2; conv_bit++) {
        uint32_t st1_conv = conv_prev_state(st2_conv, conv_bit);
        if (2 * conv_output(0, st1_conv, curr_conv_bit) +
                conv_output(1, st1_conv, curr_conv_bit) ==
            st2_crf_base) {
          prev_state_info.st_conv = st1_conv;
          prev_state_info.st_crf = st1_crf;
          prev_state_info.post_idx_0 = to_idx_crf_in_post(st2_crf);
          prev_state_info.post_idx_1 = st1_crf;
          prev_state_info.msg_shift = 1;
          prev_state_info.msg_newbits = curr_conv_bit;
          prev_state_vec.push_back(prev_state_info);
        }
      }
    } else {
      for (uint8_t conv_bit = 0; conv_bit < 2; conv_bit++) {
        for (uint8_t conv_bit_1 = 0; conv_bit_1 < 2; conv_bit_1++) {
          uint32_t st1_5_conv = conv_prev_state(st2_conv, conv_bit);
          uint32_t st1_conv = conv_prev_state(st1_5_conv, conv_bit_1);
          uint8_t bit_0, bit_1, bit_2, bit_3;
          bit_0 = conv_output(0, st1_conv, curr_conv_bit_1);
          bit_1 = conv_output(1, st1_conv, curr_conv_bit_1);
          bit_2 = conv_output(0, st1_5_conv, curr_conv_bit);
          bit_3 = conv_output(1, st1_5_conv, curr_conv_bit);
          uint8_t base = 0;
          switch (punc_pattern) {
            case 1:
              base = rc_flag ? (2 * bit_2 + bit_1) : (2 * bit_1 + bit_2);
              break;
            case 2:
              base = rc_flag ? (2 * bit_3 + bit_0) : (2 * bit_0 + bit_3);
              break;
            case 3:
              base = rc_flag ? (2 * bit_3 + bit_1) : (2 * bit_1 + bit_3);
              break;
          }
          if (base == st2_crf_base) {
            prev_state_info.st_conv = st1_conv;
            prev_state_info.st_crf = st1_crf;
            prev_state_info.post_idx_0 = to_idx_crf_in_post(st2_crf);
            prev_state_info.post_idx_1 = st1_crf;
            prev_state_info.msg_shift = 2;
            prev_state_info.msg_newbits = 2 * curr_conv_bit_1 + curr_conv_bit;
            prev_state_vec.push_back(prev_state_info);
          }
        }
      }
    }
  }
  prev_state_vec.shrink_to_fit();
  return prev_state_vec;
}

bool is_valid_state(const uint32_t &msg_pos, const uint32_t &st_conv,
                    const uint32_t &msg_len) {
  bool valid_state = true;
  for (uint32_t shift = 0; shift < mem_conv; shift++) {
    int64_t pos_in_msg =
        (int64_t)(msg_pos)-1 -
        (int64_t)shift;  // can be from -mem_conv to msg_len+mem_conv-1
    int64_t pos_in_msg_fwd = pos_in_msg;  // for sync marker, convert to forward
                                          // strand position if rc_flag
    if (rc_flag) pos_in_msg_fwd = msg_len - 1 - pos_in_msg;
    bool bit_at_shift = (st_conv >> (mem_conv - 1 - shift)) & 1;
    if (pos_in_msg < 0) {
      // initial state should match
      if (bit_at_shift !=
          ((initial_state_conv >> (mem_conv + pos_in_msg)) & 1)) {
        valid_state = false;
        break;
      }
    } else if (pos_in_msg >= msg_len) {
      // final state
      if (bit_at_shift != ((final_state_conv >> (pos_in_msg - msg_len)) & 1)) {
        valid_state = false;
        break;
      }
    } else if (sync_marker_length > 0 &&
               pos_in_msg_fwd % sync_marker_period < sync_marker_length) {
      // sync marker should match
      if (bit_at_shift != sync_marker[pos_in_msg_fwd % sync_marker_period]) {
        valid_state = false;
        break;
      }
    }
  }
  return valid_state;
}
