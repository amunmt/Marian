#pragma once

#include "graph/expression_graph.h"
#include "fbgemm/packed_gemm.h"
#include "tensors/cpu/integer_common.h"

#include <unordered_map>

namespace marian {
  namespace cpu {
    void Transpose10(marian::Tensor out, const marian::Tensor in);
  }
}

namespace {

  std::string getSubstringName(std::string& in) {
    std::string::size_type pos = in.find("_QuantMultA");
    if (pos != std::string::npos) {
      return in.substr(0, pos);
    } else {
      return "";
    }
  }
}

namespace marian {


// When FBGEMM based packed GEMM is used, some weight matrices need to be packed offline.
// The decision which weights can be packed or not should be done walking through the graph.
// This requires some more changes, but we temporarily do this just by name ("_W") of the weights.
// And, this introduces a low level packed_gemm.h apis interact with high level graph class.
// So, we make a subclass of ExpressionGraph and put those immature codes in this class.
// We will improve this in the near future. 
class ExpressionGraphPackable : public ExpressionGraph {
public:
  ExpressionGraphPackable()
    : ExpressionGraph( /* inference =  */ true) {} // Packable expression graph only supports inference

  virtual ~ExpressionGraphPackable() {}

  // Find QuanMults packed together with the model.
  // Strip the quantMult bit of the name so we can easily match them with the tensor name
  std::unordered_map<std::string, marian::Tensor> getQMults(std::map<std::string, marian::Expr>& inmap) {
    std::unordered_map<std::string, marian::Tensor> ret;
    for (auto&& p : inmap) {
      std::string pName = p.first;
      if (!namespace_.empty()) {
        if (pName.substr(0, namespace_.size() + 2) == namespace_ + "::")
          pName = pName.substr(namespace_.size() + 2);
      }
      std::string substring_only = getSubstringName(pName);
      if (substring_only != "") {
        ret.insert({substring_only, p.second->val()});
      }
    }
    return ret;
  }

  // Convert model weights into packed format and save to IO items.
  // @TODO: review this
  void packAndSave(const std::string& name, const std::string& meta, Type gemmElementType = Type::float32, Type saveElementType = Type::float32) {
    std::vector<io::Item> ioItems;

    auto qMultMap = getQMults(params()->getMap());
    // sorted by name in std::map
    for (auto p : params()->getMap()) {
      std::string pName = p.first;

      if (!namespace_.empty()) {
        if (pName.substr(0, namespace_.size() + 2) == namespace_ + "::")
          pName = pName.substr(namespace_.size() + 2);
      }

      Tensor val = p.second->val();

      // save as packed format
      // @TODO Hardcoded to find packable weights
      // int8 - all the weights used for affine op and dot op
      // fp16 - all the weights used for affine op
      if ((gemmElementType == Type::packed8avx2 || gemmElementType == Type::packed8avx512)
        && (pName.find("_W") == pName.length() - 3 || pName.find("_W") == pName.length() - 2)) {
#if USE_FBGEMM
        using namespace marian::cpu::variant;
        // packing information - size
        int nrow;
        int ncol;
        uint64_t packsize;

        fbgemmPacked8PackInfo(val->shape(),
                              gemmElementType,
                              pName.find("Wemb") != std::string::npos,
                              nrow,
                              ncol,
                              packsize);

        auto allocator = New<TensorAllocator>(getBackend());

        // buffer tensor to save packed matrix
        Tensor packedTensor;
        allocator->allocate(packedTensor, { 1, (int32_t)packsize }, Type::uint8);

        //Pack B matrix into int8
        fbgemmPacked8Pack(packedTensor,
                          val->data(),
                          gemmElementType,
                          pName.find("Wemb") != std::string::npos,
                          nrow,
                          ncol,
                          packsize);
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = gemmElementType;

        // Use the actual memory as this will be aligned and padded.
        // When memory mapping this is required. Shape keeps track of
        // tensor size. Saving to *.npz will cut to size.
        auto mem = packedTensor->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());

        ioItems.emplace_back(std::move(item));
#else
        ABORT("Packed type {} only supported when compiled with -DUSE_FBGEMM=on", gemmElementType);
#endif
      // fp16 quantization option
      } else if (gemmElementType == Type::packed16 && pName.find("_W") == pName.length() - 3) {
#if USE_FBGEMM
        using namespace marian::cpu::variant;

        // packing information
        int nrow, ncol, kernel_ncol_blocks, brow, bcol, last_brow, nbrow, nbcol;
        uint64_t packsize;

        fbgemmPacked16PackInfo(val->shape(),
          false,
          nrow,
          ncol,
          kernel_ncol_blocks,
          brow,
          bcol,
          last_brow,
          nbrow,
          nbcol,
          packsize);

        auto allocator = New<TensorAllocator>(getBackend());

        Tensor packedTensor;
        allocator->allocate(packedTensor, { 1, (int32_t)packsize }, Type::uint8);

        // fbgemmPacked16Pack
        fbgemmPacked16Pack(packedTensor,
          val->data(),
          false,
          nrow,
          ncol,
          kernel_ncol_blocks,
          brow,
          bcol,
          last_brow,
          nbrow,
          nbcol,
          packsize);
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = Type::packed16;

        // Use the actual memory as this will be aligned and padded.
        // When memory mapping this is required. Shape keeps track of
        // tensor size. Saving to *.npz will cut to size.
        auto mem = packedTensor->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());

        ioItems.emplace_back(std::move(item));
#else
        ABORT("Packed type {} only supported when compiled with -DUSE_FBGEMM=on", gemmElementType);
#endif
      } else if (isIntgemm(gemmElementType) && pName.find("QuantMultA") == std::string::npos &&
      (pName.find("_W") == pName.length() - 3 || pName.find("_W") == pName.length() - 2 ||
      (pName.find("Wemb") != std::string::npos && (gemmElementType == Type::intgemm8 || gemmElementType == Type::intgemm16)))) {
#if COMPILE_CPU  // When compressing Wemb we only work with intgemm8 or intgemm16 agnostic types as we don't have unprepareB
        using cpu::integer::cols;
        using cpu::integer::rows;
        auto allocator = New<TensorAllocator>(getBackend());

        Tensor paramMat; //This allocates extra 4 bytes at the end because of gemmElementType
        allocator->allocate(paramMat, val->shape(), gemmElementType);

        // Compute QuantMultiplier, compress matrix and store quantMult at the end.
        // We need to tranpose first, because of our architecture independet format requiring a transposed matrix
        Tensor tmp;
        allocator->allocate(tmp, val->shape(), val->type());
        if (pName.find("Wemb") == std::string::npos) {
          cpu::Transpose10(tmp, val);
        } else {
          tmp = val; // The Wemb matrix is always transposed
        }
  
        if(sizeOf(gemmElementType) == 1) { // is 8-bit Intgemm type
          float quantMult = cpu::integer::computeQuantMult<Type::intgemm8>(val);

          // Hardware-specific conversions which allow to implement memory-mapping and avoid conversion at runtime
          cpu::integer::passOrAbort(gemmElementType); // Check if the hardware supports the GEMM type
          if(isSsse3(gemmElementType)) {
            intgemm::ssse3::Kernels8::PrepareBTransposed(tmp->data(), /*input*/
                                                    paramMat->data<int8_t>(), /*output*/
                                                    quantMult, /*Quant Mult*/
                                                    rows(val),
                                                    cols(val));
          } else if(isAvx2(gemmElementType)) {
            intgemm::avx2::Kernels8::PrepareBTransposed(tmp->data(), /*input*/
                                                   paramMat->data<int8_t>(), /*output*/
                                                   quantMult, /*Quant Mult*/
                                                   rows(val),
                                                   cols(val));
          } else if(isAvx512(gemmElementType)) {
            intgemm::avx512bw::Kernels8::PrepareBTransposed(tmp->data(), /*input*/
                                                     paramMat->data<int8_t>(), /*output*/
                                                     quantMult, /*Quant Mult*/
                                                     rows(val),
                                                     cols(val));
          } else {
            ABORT_IF(gemmElementType != Type::intgemm8, "Type {} is not supported", gemmElementType); // shouldn't really happen, but let's make sure
            intgemm::Int8::PrepareA(tmp->data(), /*input*/
                                    paramMat->data<int8_t>(), /*output*/
                                    quantMult, /*Quant Mult*/
                                    rows(val),
                                    cols(val));
          }
          //Put the quantMult at the back of the tensor
          cpu::integer::getQuantMult<Type::intgemm8>(paramMat) = quantMult;
          // Try to find QuanMultA if it exists
          auto qMultAPair = qMultMap.find(pName);
          if (qMultAPair != qMultMap.end()) {
            float aQuantMult = *qMultAPair->second->data();
            cpu::integer::getQuantMultA<Type::intgemm8>(paramMat) = aQuantMult;
          }

        } else if(sizeOf(gemmElementType) == 2) { // is 16-bit Intgemm type
          float quantMult = cpu::integer::computeQuantMult<Type::intgemm16>(val);

           // Hardware-specific conversions which allow to implement memory-mapping and avoid conversion at runtime
           cpu::integer::passOrAbort(gemmElementType); // Check if the hardware supports the GEMM type
          if(isSse2(gemmElementType)) {
            intgemm::sse2::Kernels16::PrepareBTransposed(tmp->data(), /*input*/
                                                    paramMat->data<int16_t>(), /*output*/
                                                    quantMult, /*Quant Mult*/
                                                    rows(val),
                                                    cols(val));
          } else if(isAvx2(gemmElementType)) {
            intgemm::avx2::Kernels16::PrepareBTransposed(tmp->data(), /*input*/
                                                    paramMat->data<int16_t>(), /*output*/
                                                    quantMult, /*Quant Mult*/
                                                    rows(val),
                                                    cols(val));
          } else if(isAvx512(gemmElementType)) {
            intgemm::avx512bw::Kernels16::PrepareBTransposed(tmp->data(), /*input*/
                                                      paramMat->data<int16_t>(), /*output*/
                                                      quantMult, /*Quant Mult*/
                                                      rows(val),
                                                      cols(val));
          } else {
            ABORT_IF(gemmElementType != Type::intgemm16, "Type {} is not supported", gemmElementType); // shouldn't really happen, but let's make sure
            intgemm::Int16::PrepareA(tmp->data(), /*input*/
                                     paramMat->data<int16_t>(), /*output*/
                                     quantMult, /*Quant Mult*/
                                     rows(val),
                                     cols(val));
          }
          //Put the quantMult at the back of the tensor
          cpu::integer::getQuantMult<Type::intgemm16>(paramMat) = quantMult;
          // Try to find QuanMultA if it exists
          auto qMultAPair = qMultMap.find(pName);
          if (qMultAPair != qMultMap.end()) {
            float aQuantMult = *qMultAPair->second->data();
            cpu::integer::getQuantMultA<Type::intgemm16>(paramMat) = aQuantMult;
          }
          
        } else {
          ABORT("Incorrect Intgemm type size: {}", sizeOf(gemmElementType));
        }

        //Save... Same as the fbgemm case
        io::Item item;
        item.name = pName;
        item.shape = val->shape();
        item.type = gemmElementType;

        auto mem = paramMat->memory();
        item.bytes.resize(mem->size());
        copy(backend_, mem->data<char>(), mem->data<char>() + mem->size(), item.bytes.data());
        ioItems.emplace_back(std::move(item));
#else
        ABORT("Packed type {} only supported when compiled with -DCOMPILE_CPU=on", gemmElementType);
#endif
      } else {
        ABORT_IF(saveElementType != Type::float32, "We currently do not know how to save matrices as {}", saveElementType);
        io::Item item;
        val->get(item, pName);
        item.convert(saveElementType);
        ioItems.emplace_back(std::move(item));
      }
    }

    if (!meta.empty())
      io::addMetaToItems(meta, "special:model.yml", ioItems);
    io::saveItems(name, ioItems);
  }
};

}  // namespace marian
