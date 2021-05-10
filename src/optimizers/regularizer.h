#pragma once

#include "common/options.h"
#include "functional/functional.h"
#include "graph/expression_graph.h"
#include "graph/expression_operators.h"
#include "layers/factory.h"
#include "tensors/backend.h"
#include "tensors/tensor.h"
#include "tensors/tensor_allocator.h"
#include "tensors/tensor_operators.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace marian {

class IRegulariser {
protected:
  Ptr<Options> options_;

  float lambda_{0.0f};
  std::string type_{""};

  std::map<std::string, Expr> partialPenalties_;

public:
  IRegulariser(Ptr<ExpressionGraph> graph, Ptr<Options> options, float lambda, std::string type)
      : options_(options), lambda_(lambda), type_(type) {}

  virtual ~IRegulariser() {}

  virtual float getLambda() { return lambda_; }

  virtual std::string getType() { return type_; }

  virtual Expr getTotalPenalty() {
    Expr totalPenalty;
    for(const auto& partialPenalty : partialPenalties_) {
      if(!totalPenalty)
        totalPenalty = partialPenalty.second;
      else
        totalPenalty = totalPenalty + lambda_ * partialPenalty.second;
    }
    return lambda_ * totalPenalty;
  }

  virtual std::map<std::string, Expr> getPartialPenalties() { return partialPenalties_; }

  virtual void clear() { partialPenalties_.clear(); }

  virtual Expr calculatePenalty(Expr W, Expr b, bool rows) = 0;
};

class LhalfRegulariser : public IRegulariser {
protected:
  Ptr<Options> options_;

public:
  LhalfRegulariser(Ptr<ExpressionGraph> graph, Ptr<Options> options, float lambda, std::string type)
      : IRegulariser(graph, options, lambda, type) {}

  // L0.5-regularisation
  // so since it is p = 0.5, parameters are sqrt and then added together with a square
  // we ignore bias in this case
  Expr calculatePenalty(Expr W, Expr b, bool rows = false) override {
    auto p = square(sum(sum(sqrt(abs(W)), -1), -2));

    partialPenalties_.emplace(W->name(), p);
    return p;
  }
};

class L1Regulariser : public IRegulariser {
protected:
  Ptr<Options> options_;

public:
  L1Regulariser(Ptr<ExpressionGraph> graph, Ptr<Options> options, float lambda, std::string type)
      : IRegulariser(graph, options, lambda, type) {}

  // L1-regularisation
  // just a sum of all absolute values
  // we ignore bias in this case
  Expr calculatePenalty(Expr W, Expr b, bool rows = false) override {
    auto p = sum(sum(abs(W), -1), -2);

    partialPenalties_.emplace(W->name(), p);
    return p;
  }
};

class L2Regulariser : public IRegulariser {
protected:
  Ptr<Options> options_;

public:
  L2Regulariser(Ptr<ExpressionGraph> graph, Ptr<Options> options, float lambda, std::string type)
      : IRegulariser(graph, options, lambda, type) {}

  // L2-regularisation
  // just a sum of square values
  // we ignore bias in this case
  Expr calculatePenalty(Expr W, Expr b, bool rows = false) override {
    auto p = sum(sum(W * W, -1), -2);

    partialPenalties_.emplace(W->name(), p);
    return p;
  }
};

class ElasticRegulariser : public IRegulariser {
protected:
  Ptr<Options> options_;

public:
  ElasticRegulariser(Ptr<ExpressionGraph> graph,
                     Ptr<Options> options,
                     float lambda,
                     std::string type)
      : IRegulariser(graph, options, lambda, type) {}

  // Elastic net regularisation
  // which is just L1 + L2
  // we ignore bias in this case
  Expr calculatePenalty(Expr W, Expr b, bool rows = false) override {
    auto p1 = sum(sum(abs(W), -1), -2);
    auto p2 = sum(sum(W * W, -1), -2);
    auto p  = p1 + p2;

    partialPenalties_.emplace(W->name(), p);
    return p;
  }
};

class GroupLassoRegulariser : public IRegulariser {
public:
  GroupLassoRegulariser(Ptr<ExpressionGraph> graph,
                        Ptr<Options> options,
                        float lambda,
                        std::string type)
      : IRegulariser(graph, options, lambda, type) {}

  Expr calculatePenalty(Expr W, Expr b, bool rows = false) override {
    Expr p;
    if(type_ == "rowcol") {
      p = rowcolPenalty(W, b, rows);
    } else if(type_ == "heads") {
      p = headPenalty(W, b, rows);
    } else if(type_ == "rowcol-root") {
      p = rowcolRootPenalty(W, b, rows);
    } else if(type_ == "layer") {
      p = layerPenalty(W, b, rows);
    }

    partialPenalties_.emplace(W->name(), p);
    return p;
  }

protected:
  Expr rowcolRootPenalty(Expr W, Expr b, bool rows = false) {
    size_t axisL2, axisL1;

    // depending on whether we regularise rows or columns, apply L1 and L2
    // alongside specific axes

    if(!rows) {
      axisL2 = -2;
      axisL1 = -1;
    } else {
      axisL2 = -1;
      axisL1 = -2;
    }

    auto WSum = sum(sqrt(abs(W)), axisL2);

    // if regularising columns, we also need to remove biases with L2
    if(!rows) {
      WSum = WSum + sqrt(abs(b));
    }

    auto p = sum(square(WSum), axisL1);

    auto scale = std::sqrt(W->shape()[0]);
    return scale * p;
  }

  Expr layerPenalty(Expr W, Expr b, bool rows = false) {
    size_t axisL2, axisL1;

    // depending on whether we regularise rows or columns, apply L1 and L2
    // alongside specific axes

    if(!rows) {
      axisL2 = -2;
      axisL1 = -1;
    } else {
      axisL2 = -1;
      axisL1 = -2;
    }

    auto WSum = sum(W * W, axisL2);

    if(!rows)
      WSum = WSum + (b * b);

    auto p = sqrt(sum(WSum, axisL1));

    auto scale = std::sqrt(W->shape()[0]);
    return scale * p;
  }

  Expr rowcolPenalty(Expr W, Expr b, bool rows = false) {
    size_t axisL2, axisL1;

    // depending on whether we regularise rows or columns, apply L1 and L2
    // alongside specific axes

    if(!rows) {
      axisL2 = -2;
      axisL1 = -1;
    } else {
      axisL2 = -1;
      axisL1 = -2;
    }

    auto WSum = sum(W * W, axisL2);

    // if regularising columns, we also need to remove biases with L2
    if(!rows) {
      WSum = WSum + (b * b);
    }

    auto p = sum(sqrt(WSum), axisL1);

    float scale = 1.0f;
    if(!rows)
      scale = std::sqrt(W->shape()[0]);
    else
      scale = std::sqrt(W->shape()[1]);

    return scale * p;
  }

  Expr headPenalty(Expr W, Expr b, bool rows = false) {
    int h = W->shape()[0];

    int blockH = W->shape()[0];                               // inner dimension = 256
    int blockW = options_->get<int>("transformer-head-dim");  // head size = 32

    int innerShape = W->shape()[0] * W->shape()[1] / (blockW * h);
    int blockNum   = W->shape()[0] * W->shape()[1] / (blockH * blockW);

    // splitting a matrix into separate heads
    // TODO: modify transformer model to split parameters first?
    // but could be inefficient matrix multiplication-wise

    auto reshaped = reshape(W, {h / blockH, blockH, innerShape, blockW});
    auto heads    = reshape(transpose(reshaped, {0, 2, 1, 3}), {1, blockNum, blockH, blockW});

    auto WSum = sum(sum(heads * heads, -2), -1);

    if(!rows) {
      auto bBlocks = reshape(b, {b->shape()[1] / blockW, 1, blockW});
      auto bSum    = sum(bBlocks * bBlocks, -1);
      WSum         = WSum + bSum;
    }

    // sum across all heads too
    auto p = sum(sqrt(WSum), -3);

    // I believe it's called orthonormalisation???
    auto scale = std::sqrt(blockH * blockW);
    return scale * p;
  }
};

class RegulariserFactory : public Factory {
public:
  using Factory::Factory;
  RegulariserFactory(Ptr<Options> options) : Factory(options){};

  Ptr<IRegulariser> construct(Ptr<ExpressionGraph> graph, float lambda, std::string type) {
    LOG_ONCE(info, "Regulariser type {}", type);
    if(type == "l1") {
      LOG_ONCE(info, "Regularisation type selected: l1");
      return New<L1Regulariser>(graph, options_, lambda, type);
    } else if(type == "l2") {
      LOG_ONCE(info, "Regularisation type selected: l2");
      return New<L2Regulariser>(graph, options_, lambda, type);
    } else if(type == "lhalf") {
      LOG_ONCE(info, "Regularisation type selected: lhalf");
      return New<LhalfRegulariser>(graph, options_, lambda, type);
    } else if(type == "elastic") {
      LOG_ONCE(info, "Regularisation type selected: elastic");
      return New<ElasticRegulariser>(graph, options_, lambda, type);
    } else if(type == "rowcol") {
      LOG_ONCE(info, "Regularisation type selected: group lasso, shape=rowcol");
      return New<GroupLassoRegulariser>(graph, options_, lambda, type);
    } else if(type == "rowcol-root") {
      LOG_ONCE(info, "Regularisation type selected: group lasso, shape=rowcol-root");
      return New<GroupLassoRegulariser>(graph, options_, lambda, type);
    } else if(type == "layer") {
      LOG_ONCE(info, "Regularisation type selected: group lasso, shape=layer");
      return New<GroupLassoRegulariser>(graph, options_, lambda, type);
    } else if(type == "heads") {
      LOG_ONCE(info, "Regularisation type selected: group lasso, shape=heads");
      return New<GroupLassoRegulariser>(graph, options_, lambda, type);
    } else {
      LOG_ONCE(
          info,
          "Regularisation type selected but not on the list? Returning nullptr, will break? {}",
          type);
      return nullptr;
    }
  }
};
}
