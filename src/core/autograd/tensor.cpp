#include "zetta/autograd/tensor.hpp"

#include <cstddef>
#include <unordered_set>

namespace zetta::autograd {

std::shared_ptr<Node> make_node(std::vector<float> data,
                                std::vector<int> shape) {
    auto node = std::make_shared<Node>();
    node->grad.assign(data.size(), 0.0F);
    node->data = std::move(data);
    node->shape = std::move(shape);
    return node;
}

Tensor Tensor::leaf(std::vector<float> data, std::vector<int> shape,
                    bool requires_grad) {
    auto node = make_node(std::move(data), std::move(shape));
    node->requires_grad = requires_grad;
    return Tensor{node};
}

void Tensor::backward() {
    // Topological order of the graph rooted at this node (parents before children).
    std::vector<std::shared_ptr<Node>> order;
    std::unordered_set<const Node*> visited;
    const std::function<void(const std::shared_ptr<Node>&)> build =
        [&](const std::shared_ptr<Node>& current) {
            if (current == nullptr || visited.count(current.get())) {
                return;
            }
            visited.insert(current.get());
            for (const auto& parent : current->parents) {
                build(parent);
            }
            order.push_back(current);
        };
    build(node_);

    // Seed: the loss is a scalar, so d(loss)/d(loss) = 1.
    node_->grad.assign(node_->data.size(), 0.0F);
    if (node_->data.size() == 1) {
        node_->grad[0] = 1.0F;
    }

    // Propagate in reverse topological order, accumulating into parents.
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if ((*it)->backward) {
            (*it)->backward((*it)->grad);
        }
    }
}

Tensor matmul(const Tensor& a, const Tensor& b) {
    const int m = a.shape()[0];
    const int k = a.shape()[1];
    const int n = b.shape()[1];

    std::vector<float> out(static_cast<std::size_t>(m) * n, 0.0F);
    for (int row = 0; row < m; ++row) {
        for (int inner = 0; inner < k; ++inner) {
            const float a_value = a.data()[row * k + inner];
            for (int col = 0; col < n; ++col) {
                out[row * n + col] += a_value * b.data()[inner * n + col];
            }
        }
    }

    auto node = make_node(std::move(out), {m, n});
    node->parents = {a.node(), b.node()};
    const std::shared_ptr<Node> a_node = a.node();
    const std::shared_ptr<Node> b_node = b.node();
    node->backward = [a_node, b_node, m, k, n](const std::vector<float>& g) {
        // dA[row, inner] += sum_col g[row, col] * B[inner, col].
        for (int row = 0; row < m; ++row) {
            for (int inner = 0; inner < k; ++inner) {
                float accumulated = 0.0F;
                for (int col = 0; col < n; ++col) {
                    accumulated += g[row * n + col] * b_node->data[inner * n + col];
                }
                a_node->grad[row * k + inner] += accumulated;
            }
        }
        // dB[inner, col] += sum_row A[row, inner] * g[row, col].
        for (int inner = 0; inner < k; ++inner) {
            for (int col = 0; col < n; ++col) {
                float accumulated = 0.0F;
                for (int row = 0; row < m; ++row) {
                    accumulated += a_node->data[row * k + inner] * g[row * n + col];
                }
                b_node->grad[inner * n + col] += accumulated;
            }
        }
    };
    return Tensor{node};
}

Tensor sum(const Tensor& a) {
    float total = 0.0F;
    for (float value : a.data()) {
        total += value;
    }
    auto node = make_node({total}, {1});
    node->parents = {a.node()};
    const std::shared_ptr<Node> a_node = a.node();
    node->backward = [a_node](const std::vector<float>& g) {
        for (float& gradient : a_node->grad) {
            gradient += g[0];
        }
    };
    return Tensor{node};
}

}  // namespace zetta::autograd
