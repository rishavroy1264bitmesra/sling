// Copyright 2017 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "myelin/flow.h"

#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>

#include "base/status.h"
#include "file/file.h"
#include "string/printf.h"

namespace sling {
namespace myelin {

std::unordered_map<string, Type> typemap = {
  {"", DT_INVALID},
  {"float16", DT_HALF},
  {"float32", DT_FLOAT},
  {"float64", DT_DOUBLE},
  {"float", DT_FLOAT},
  {"bfloat16", DT_BFLOAT16},
  {"int8", DT_INT8},
  {"int16", DT_INT16},
  {"int32", DT_INT32},
  {"int64", DT_INT64},
  {"uint8", DT_UINT8},
  {"uint16", DT_UINT16},
  {"bool", DT_BOOL},
  {"string", DT_STRING},
  {"complex64", DT_COMPLEX64},
  {"complex128", DT_COMPLEX128},
  {"qint8", DT_QINT8},
  {"qint32", DT_QINT32},
  {"qint16", DT_QINT16},
  {"quint8", DT_QUINT8},
  {"quint16", DT_QUINT16},
  {"resource", DT_RESOURCE},
};

std::vector<TypeTraits> typetraits = {
  {DT_INVALID, "void", 0},
  {DT_FLOAT, "float32", sizeof(float)},
  {DT_DOUBLE, "float64", sizeof(double)},
  {DT_INT32, "int32", sizeof(int32_t)},
  {DT_UINT8, "uint8", sizeof(uint8_t)},
  {DT_INT16, "int16", sizeof(int16_t)},
  {DT_INT8, "int8", sizeof(int8_t)},
  {DT_STRING, "string", sizeof(char *)},
  {DT_COMPLEX64, "complex64", 2 * sizeof(double)},
  {DT_INT64, "int64", sizeof(int64_t)},
  {DT_BOOL, "bool", sizeof(bool)},
  {DT_QINT8, "qint8", sizeof(int8_t)},
  {DT_QUINT8, "quint8", sizeof(uint8_t)},
  {DT_QINT32, "qint32", sizeof(int32_t)},
  {DT_BFLOAT16, "bfloat16", 16},
  {DT_QINT16, "qint16", sizeof(int16_t)},
  {DT_UINT16, "uint16", sizeof(uint16_t)},
  {DT_QUINT16, "quint16", sizeof(uint16_t)},
  {DT_COMPLEX128, "complex128", 2 * sizeof(float)},
  {DT_HALF, "float16", 2},
  {DT_RESOURCE, "resource", 1},
};

bool Shape::IsSameSize(const Shape &other) const {
  if (rank() != other.rank()) return false;
  for (int d = 0; d < rank(); ++d) {
    if (dim(d) != other.dim(d) && dim(d) != -1 && other.dim(d) != -1) {
      return false;
    }
  }
  return true;
}

string Shape::ToString() const {
  string str;
  for (int d = 0; d < rank(); ++d) {
    if (d > 0) str.append("x");
    if (dim(d) == -1) {
      str.append("?");
    } else {
      StringAppendF(&str, "%d", dim(d));
    }
  }
  return str;
}

const TypeTraits &TypeTraits::of(Type type) {
  return typetraits[type];
}

const TypeTraits &TypeTraits::of(string &name) {
  auto f = typemap.find(name);
  return f != typemap.end() ? typetraits[f->second] : typetraits[DT_INVALID];
}

string TypeTraits::str(void *data) const {
  if (data == nullptr) return "null";
  switch (type_) {
    case DT_INT8:
      return std::to_string(*reinterpret_cast<int8 *>(data));

    case DT_INT16:
      return std::to_string(*reinterpret_cast<int16 *>(data));

    case DT_INT32:
      return std::to_string(*reinterpret_cast<int32 *>(data));

    case DT_INT64:
      return std::to_string(*reinterpret_cast<int64 *>(data));

    case DT_UINT8:
      return std::to_string(*reinterpret_cast<uint8 *>(data));

    case DT_UINT16:
      return std::to_string(*reinterpret_cast<uint16 *>(data));

    case DT_FLOAT:
      return std::to_string(*reinterpret_cast<float *>(data));

    case DT_DOUBLE:
      return std::to_string(*reinterpret_cast<double *>(data));

    default:
      return "???";
  }
}

Transformations::~Transformations() {
  for (auto *t : typers_) delete t;
}

// Flow file parser.
class Parser {
 public:
  // Initialize parser with input buffer.
  Parser(char *ptr, char *end) : ptr_(ptr), end_(end) {}

  // Get data buffer from input and advance the current input pointer.
  char *Get(int len) {
    CHECK_LE(len, end_ - ptr_) << "Unexpected end of input";
    char *p = ptr_;
    ptr_ += len;
    return p;
  }

  // Get next integer from input.
  int GetInt() {
    return *reinterpret_cast<int *>(Get(4));
  }

  // Get next 64-bit integer from input.
  uint64_t GetLong() {
    return *reinterpret_cast<uint64_t *>(Get(8));
  }

  // Get next length-prefixed string from input.
  string GetString() {
    int len = GetInt();
    char *str = Get(len);
    return string(str, len);
  }

 private:
  char *ptr_;  // current position
  char *end_;  // end of input buffer
};

void Flow::Variable::AddAlias(const string &alias) {
  if (std::find(aliases.begin(), aliases.end(), alias) == aliases.end()) {
    aliases.push_back(alias);
  }
}

string Flow::Variable::TypeString() const {
  string str;
  if (ref) str.append("&");
  str.append(TypeTraits::of(type).name());
  if (!shape.scalar()) {
    str.append("[");
    str.append(shape.ToString());
    str.append("]");
  }
  return str;
}

void Flow::Operation::AddInput(Variable *var) {
  inputs.push_back(var);
  var->consumers.push_back(this);
}

void Flow::Operation::AddOutput(Variable *var) {
  outputs.push_back(var);
  CHECK(var->producer == nullptr);
  var->producer = this;
}

const string &Flow::Operation::GetAttr(const string &name) {
  static string empty;
  for (auto &attr : attrs) {
    if (attr.name == name) return attr.value;
  }
  return empty;
}

int Flow::Operation::GetAttr(const string &name, int defval) {
  for (auto &attr : attrs) {
    if (attr.name == name) return atoi(attr.value.c_str());
  }
  return defval;
}

void Flow::Function::AddOperation(Operation *op) {
  CHECK(op->func == nullptr);
  op->func = this;
  ops.push_back(op);
}

void Flow::Connector::AddLink(Variable *var) {
  links.push_back(var);
}

Flow::Flow() {}

Flow::~Flow() {
  for (auto *op : ops_) delete op;
  for (auto *var : vars_) delete var;
  for (auto *func : funcs_) delete func;
  for (auto *cnx : cnxs_) delete cnx;
  for (auto *ptr : memory_) free(ptr);
}

Status Flow::Load(const string &filename) {
  // Load flow file into memory.
  File *file;
  Status st = File::Open(filename, "r", &file);
  if (!st.ok()) return st;
  uint64 size;
  CHECK_OK(file->GetSize(&size));
  char *data = static_cast<char *>(malloc(size));
  memory_.push_back(data);
  file->ReadOrDie(data, size);
  CHECK_OK(file->Close());

  // Read header.
  Parser parser(data, data + size);
  int magic = parser.GetInt();
  CHECK_EQ(magic, 0x776f6c66) << filename << " is not a flow file";
  int version = parser.GetInt();
  CHECK_EQ(version, 3) << "unsupported flow file version";

  // Read variables.
  int num_vars = parser.GetInt();
  for (int i = 0; i < num_vars; ++i) {
    // Create new variable.
    Variable *var = new Variable;
    vars_.push_back(var);

    // Get variable name.
    var->name = parser.GetString();

    // Get aliases.
    int num_aliases = parser.GetInt();
    for (int i = 0; i < num_aliases; ++i) {
      var->aliases.push_back(parser.GetString());
    }

    // Get variable type.
    string type = parser.GetString();
    if (type.empty()) {
      var->type = DT_INVALID;
    } else {
      if (type[0] == '&') {
        var->ref = true;
        type.erase(0, 1);
      }
      const TypeTraits &t = TypeTraits::of(type);
      CHECK(t.valid()) << "Unknown type: " << type;
      var->type = t.type();
    }

    // Get variable shape.
    int rank = parser.GetInt();
    for (int d = 0; d < rank; ++d) {
      int size = parser.GetInt();
      var->shape.add(size == -1 ? batch_size_ : size);
    }

    // Get optional variable constant.
    var->size = parser.GetLong();
    if (var->size != 0) var->data = parser.Get(var->size);
  }

  // Read operations.
  int num_ops = parser.GetInt();
  for (int i = 0; i < num_ops; ++i) {
    // Create new operation.
    Operation *op = new Operation;
    ops_.push_back(op);

    // Get operation name and type.
    op->name = parser.GetString();
    op->type = parser.GetString();

    // Get inputs.
    int num_inputs = parser.GetInt();
    for (int j = 0; j < num_inputs; ++j) {
      string input = parser.GetString();
      Variable *var = Var(input);
      CHECK(var != nullptr) << "Unknown input: " << input;
      op->inputs.push_back(var);
      var->consumers.push_back(op);
    }

    // Get outputs.
    int num_outputs = parser.GetInt();
    for (int j = 0; j < num_outputs; ++j) {
      string output = parser.GetString();
      Variable *var = Var(output);
      CHECK(var != nullptr) << "Unknown " << op->name << " output: " << output;
      op->outputs.push_back(var);
      var->AddAlias(op->name);
      CHECK(var->producer == nullptr) << "Multiple producers for " << var->name;
      var->producer = op;
    }

    // Get attributes.
    int num_attrs = parser.GetInt();
    for (int j = 0; j < num_attrs; ++j) {
      string name = parser.GetString();
      string value = parser.GetString();
      op->attrs.emplace_back(name, value);
      if (name == "task") op->task = std::stoi(value);
    }
  }

  // Read functions.
  int num_funcs = parser.GetInt();
  for (int i = 0; i < num_funcs; ++i) {
    // Create new function.
    Function *func = new Function;
    funcs_.push_back(func);

    // Get function name.
    func->name = parser.GetString();

    // Get function ops.
    int num_ops = parser.GetInt();
    for (int j = 0; j < num_ops; ++j) {
      string opname = parser.GetString();
      Operation *op = Op(opname);
      CHECK(op != nullptr) << "Unknown op: " << opname;
      func->AddOperation(op);
    }
  }

  // Read connectors.
  int num_cnxs = parser.GetInt();
  for (int i = 0; i < num_cnxs; ++i) {
    // Create new connector.
    Connector *cnx = new Connector;
    cnxs_.push_back(cnx);

    // Get connector name.
    cnx->name = parser.GetString();

    // Get connector links.
    int num_links = parser.GetInt();
    for (int j = 0; j < num_links; ++j) {
      string varname = parser.GetString();
      Variable *var = Var(varname);
      CHECK(var != nullptr) << "Unknown variable: " << varname;
      cnx->AddLink(var);
    }
  }

  return Status::OK;
}

void Flow::Analyze(const Transformations &transformations) {
  InferInputsAndOutputs();
  Transform(transformations);
  Sort();
  InferTypes(transformations);
}

void Flow::InferInputsAndOutputs() {
  for (Variable *var : vars_) {
    // Check the input and output attributes of the producing op.
    bool input_set = false;
    bool output_set = false;
    if (var->producer != nullptr) {
      const string &input = var->producer->GetAttr("input");
      if (!input.empty()) {
        if (input == "1" || input == "true") var->function_input = true;
        input_set = true;
      }
      const string &output = var->producer->GetAttr("output");
      if (!output.empty()) {
        if (output == "1" || output == "true") var->function_output = true;
        output_set = true;
      }
    }

    // A variable which has no producer or where the producer has no inputs
    // is considered an input to the function.
    if (!input_set) {
      if (var->producer == nullptr || var->producer->inputs.empty()) {
        var->function_input = true;
      }
    }

    // A variable which has no consumers is considered an output for the
    // function.
    if (!output_set) {
      if (var->consumers.empty()) {
        var->function_output = true;
      }
    }
  }
}

void Flow::Transform(const Transformations &transformations) {
  // Eliminate Identity ops by moving the inputs to the output.
  std::vector<Operation *> noops;
  for (const string &identity : transformations.noops()) {
    for (Operation *op : ops_) {
      if (op->type == identity) noops.push_back(op);
    }
  }

  // Remove no-ops from the flow and eliminate the intermediate variables.
  for (Operation *op : noops) {
    Eliminate(op);
  }

  // Combine ops.
  for (const auto &c : transformations.combinations()) {
    Combine(c.first, c.second, c.replacement);
  }
}

void Flow::Combine(const string &first,
                   const string &second,
                   const string &combined) {
  // Find operations that can be combined.
  bool again = true;
  while (again) {
    again = false;
    for (Operation *op : ops_) {
      if (op->type != first) continue;
      if (op->outputs.size() != 1) continue;
      Variable *var = op->outputs[0];
      if (var->consumers.size() != 1) continue;
      if (var->consumers[0]->type != second) continue;
      if (var->consumers[0]->task != op->task) continue;

      Merge(op, var->consumers[0], combined);
      again = true;
    }
  }
}

Flow::Operation *Flow::Merge(Operation *first,
                             Operation *second,
                             const string &combined) {
  // Check that ops can be merged.
  CHECK_EQ(first->outputs.size(), 1);
  Variable *var = first->outputs[0];
  CHECK_EQ(var->consumers.size(), 1);
  CHECK(var->consumers[0] == second);

  // Add inputs for second op to the first/combined op.
  for (Variable *v : second->inputs) {
    if (v != var) {
      first->inputs.push_back(v);
      for (Operation *&c : v->consumers) {
        if (c == second) c = first;
      }
    }
  }

  // Add outputs from second op to the first/combined op.
  first->outputs.clear();
  for (Variable *v : second->outputs) {
    first->outputs.push_back(v);
    v->producer = first;
  }

  // Set operation type for the first to the combined type.
  first->type = combined;

  // Delete second operation.
  DeleteOperation(second);

  // Delete intermediate variable.
  DeleteVariable(var);

  return first;
}

void Flow::Eliminate(Operation *op) {
  if (op->inputs.size() > 0) {
    // Update all usages of output to use the input variable instead.
    CHECK_EQ(op->inputs.size(), 1);
    CHECK_EQ(op->outputs.size(), 1);
    Variable *input = op->inputs[0];
    Variable *output = op->outputs[0];
    if (output->function_input) input->function_input = true;
    if (output->function_output) input->function_output = true;
    for (Operation *target : ops_) {
      for (int i = 0; i < target->inputs.size(); ++i) {
        if (target->inputs[i] == output) {
          target->inputs[i] = input;
        }
      }
    }

    // Remove op as consumer of input variable.
    auto f = std::find(input->consumers.begin(), input->consumers.end(), op);
    CHECK(f != input->consumers.end());
    input->consumers.erase(f);

    // Move consumers of output variable to input variable.
    for (Operation *consumer : output->consumers) {
      input->consumers.push_back(consumer);
    }

    // Make input variable an alias for the output variable.
    input->AddAlias(output->name);
    for (const string &alias : output->aliases) {
      input->AddAlias(alias);
    }

    // Delete output variable.
    DeleteVariable(output);
  } else {
    // Clear producer for outputs.
    for (Variable *var : op->outputs) var->producer = nullptr;
  }

  // Delete operation.
  DeleteOperation(op);
}

static bool CompareOpOrder(Flow::Operation *o1, Flow::Operation *o2) {
  return o1->order < o2->order;
}

struct PriorityComparator {
  bool operator ()(Flow::Operation *o1, Flow::Operation *o2) {
    if (o1->priority == o2->priority) {
      return o1->order < o2->order;
    } else {
      return o1->priority > o2->priority;
    }
  }
};

void Flow::Sort() {
  // Set priority for each operation. Operations that other tasks depend on are
  // scheduled early and operations that depend on other tasks are scheduled
  // late in other to allow for as much parallelism as possible.
  // The operations are assigned the following priorities:
  //   4: operations that parallel operations depend on.
  //   3: operations with no dependencies to parallel operations.
  //   2: parallel operation.
  //   1: operations that depend on parallel operations.
  std::unordered_set<Operation *> pre;
  std::unordered_set<Operation *> post;
  for (Operation *op : ops_) {
    if (op->task != 0) {
      // Parallel operation.
      op->priority = 2;

      // Add input to parallel operation to pre-parallel phase.
      for (Variable *var : op->inputs) {
        if (var->producer != nullptr && var->producer->task == 0) {
          var->producer->priority = 4;
          pre.insert(var->producer);
        }
      }

      // Add output from parallel operation to post-parallel phase.
      for (Variable *var : op->outputs) {
        for (Operation *consumer : var->consumers) {
          if (consumer->task == 0) {
            consumer->priority = 1;
            post.insert(consumer);
          }
        }
      }
    }
  }
  bool again = true;
  while (again) {
    again = false;

    // Expand the pre-parallel phase.
    for (Operation *op : pre) {
      for (Variable *var : op->inputs) {
        if (var->producer != nullptr && pre.count(var->producer) == 0) {
          var->producer->priority = 4;
          pre.insert(var->producer);
          again = true;
        }
      }
    }

    // Expand the post-parallel phase.
    for (Operation *op : post) {
      for (Variable *var : op->outputs) {
        for (Operation *consumer : var->consumers) {
          if (consumer->task == 0 && post.count(consumer) == 0) {
            consumer->priority = 1;
            post.insert(consumer);
            again = true;
          }
        }
      }
    }
  }

  // Operations and variables in prioritized execution order.
  std::vector<Operation *> ordered_ops;
  std::vector<Variable *> ordered_vars;

  // Add all variables with no producer.
  for (Variable *var : vars_) {
    if (var->producer == nullptr) ordered_vars.push_back(var);
  }

  // Compute the number of missing inputs for each operation and add operations
  // that do not depend on other operations to the ready queue.
  typedef std::vector<Operation *> Operations;
  std::priority_queue<Operation *, Operations, PriorityComparator> ready;
  int order = 0;
  for (Operation *op : ops_) {
    for (Variable *var : op->inputs) {
      if (var->producer != nullptr) op->missing++;
    }
    if (op->missing == 0) {
      op->order = order++;
      ready.push(op);
    }
  }

  // Keep adding ops that are ready to be computed.
  while (!ready.empty()) {
    // Get the next op with highest priority that is ready.
    Operation *op = ready.top();
    ready.pop();

    // Add it to the ordered set of ops.
    ordered_ops.push_back(op);

    // Propagate readiness to consumers.
    for (Variable *o : op->outputs) {
      ordered_vars.push_back(o);
      for (Operation *consumer : o->consumers) {
        CHECK_NE(consumer->missing, 0);
        if (--consumer->missing == 0) {
          consumer->order = order++;
          ready.push(consumer);
        }
      }
    }
  }

  CHECK_EQ(vars_.size(), ordered_vars.size());
  vars_.swap(ordered_vars);

  CHECK_EQ(ops_.size(), ordered_ops.size());
  ops_.swap(ordered_ops);

  // Set order for ops.
  for (int i = 0; i < ops_.size(); ++i) {
    ops_[i]->order = i;
  }

  // Sort ops for functions.
  for (Function *func : funcs_) {
    std::sort(func->ops.begin(), func->ops.end(), CompareOpOrder);
  }
}

bool Flow::InferTypes(const Transformations &transformations) {
  // Assume that operations has been topologically ordered so the inputs for
  // an operation come before the operation itself.
  int num_unresolved = 0;
  int num_skipped = 0;
  for (Operation *op : ops_) {
    // Check that all inputs have type information.
    bool missing = false;
    for (Variable *input : op->inputs) {
      if (input->type == DT_INVALID) {
        missing = true;
        LOG(WARNING) << "Skipping type inference for " << op->name
                     << " because input " << input->name
                     << " is missing type";
      }
      if (input->shape.undefined()) {
        missing = true;
        LOG(WARNING) << "Skipping type inference for " << op->name
                     << " because input " << input->name
                     << " is missing shape";
      }
    }
    if (missing) {
      num_skipped++;
      continue;
    }

    // Check if any of the outputs are missing type or shape information.
    bool infer = false;
    for (Variable *output : op->outputs) {
      if (output->type == DT_INVALID || output->shape.undefined()) infer = true;
    }
    if (!infer) continue;

    // Try to infer type and shape for operation outputs.
    for (Typer *typer : transformations.typers()) {
      bool done = typer->InferTypes(op);
      if (done) break;
    }

    // Check that all outputs are now resolved.
    bool resolved = true;
    for (Variable *output : op->outputs) {
      if (output->type == DT_INVALID) {
        LOG(WARNING) << "Variable " << output->name << " is missing type";
        resolved = false;
      }
      if (output->shape.undefined()) {
        LOG(WARNING) << "Variable " << output->name << " is missing shape";
        resolved = false;
      }
    }
    if (!resolved) num_unresolved++;
  }

  if (num_unresolved > 0 || num_skipped > 0) {
    LOG(WARNING) << (num_unresolved + num_skipped) << " ops with unresolved"
                 << " types, " << num_skipped << " skipped";
    return false;
  }

  return true;
}

Flow::Variable *Flow::AddVariable(const string &name,
                                  Type type,
                                  const Shape &shape) {
  Variable *var = new Variable;
  vars_.push_back(var);
  var->name = name;
  var->type = type;
  var->shape = shape;
  return var;
}

Flow::Operation *Flow::AddOperation(const string &name,
                                    const string &type) {
  Operation *op = new Operation;
  ops_.push_back(op);
  op->name = name;
  op->type = type;
  return op;
}

Flow::Operation *Flow::AddOperation(Function *func,
                                    const string &name,
                                    const string &type) {
  Operation *op = AddOperation(name, type);
  func->AddOperation(op);
  return op;
}

Flow::Function *Flow::AddFunction(const string &name) {
  Function *func = new Function;
  funcs_.push_back(func);
  func->name = name;
  return func;
}

Flow::Connector *Flow::AddConnector(const string &name) {
  Connector *cnx = new Connector;
  cnxs_.push_back(cnx);
  cnx->name = name;
  return cnx;
}

void Flow::DeleteVariable(Variable *var) {
  auto f = std::find(vars_.begin(), vars_.end(), var);
  if (f != vars_.end()) vars_.erase(f);
  delete var;
}

void Flow::DeleteOperation(Operation *op) {
  Function *func = op->func;
  if (func != nullptr) {
    auto f = std::find(func->ops.begin(), func->ops.end(), op);
    if (f != func->ops.end()) func->ops.erase(f);
  }
  auto f = std::find(ops_.begin(), ops_.end(), op);
  if (f != ops_.end()) ops_.erase(f);
  delete op;
}

string Flow::ToString() const {
  string str;
  for (const Variable *var : vars_) {
    StringAppendF(&str, "var %s : %s",
                  var->name.c_str(),
                  var->TypeString().c_str());
    if (var->data != nullptr) {
      StringAppendF(&str, ", %lu bytes", var->size);
    }
    StringAppendF(&str, " {\n");
    if (var->producer != nullptr) {
      StringAppendF(&str, "  from %s\n", var->producer->name.c_str());
    }
    for (const Operation *op : var->consumers) {
      StringAppendF(&str, "  to %s\n", op->name.c_str());
    }
    if (var->function_input) StringAppendF(&str, "  function input\n");
    if (var->function_output) StringAppendF(&str, "  function output\n");
    for (const string &alias : var->aliases) {
      if (alias != var->name) {
        StringAppendF(&str, "  aka %s\n", alias.c_str());
      }
    }
    str.append("}\n\n");
  }
  for (const Operation *op : ops_) {
    StringAppendF(&str, "op %s : %s {\n", op->name.c_str(), op->type.c_str());
    if (op->task != 0) {
      StringAppendF(&str, "  task %d\n", op->task);
    }
    for (const Variable *input : op->inputs) {
      StringAppendF(&str, "  input %s : %s\n",
                    input->name.c_str(),
                    input->TypeString().c_str());
    }
    for (const Variable *output : op->outputs) {
      StringAppendF(&str, "  output %s : %s\n",
                    output->name.c_str(),
                    output->TypeString().c_str());
    }
    for (const Attribute &attr : op->attrs) {
      if (attr.value.size() > 128) {
        StringAppendF(&str, "  %s = <<%lu bytes>>\n",
                      attr.name.c_str(),
                      attr.value.size());
      } else {
        StringAppendF(&str, "  %s = %s\n",
                      attr.name.c_str(),
                      attr.value.c_str());
      }
    }
    str.append("}\n\n");
  }
  for (const Function *func : funcs_) {
    StringAppendF(&str, "func %s {\n", func->name.c_str());
    for (const Operation *op : func->ops) {
      StringAppendF(&str, "  %s : %s\n", op->name.c_str(), op->type.c_str());
    }
    StringAppendF(&str, "}\n\n");
  }

  for (const Connector *cnx : cnxs_) {
    StringAppendF(&str, "connector %s {\n", cnx->name.c_str());
    for (const Variable *link : cnx->links) {
      StringAppendF(&str, "  %s : %s\n",
                    link->name.c_str(),
                    link->TypeString().c_str());
    }
    StringAppendF(&str, "}\n\n");
  }

  return str;
}

Flow::Variable *Flow::Var(const string &name) {
  for (Variable *var : vars_) {
    if (var->name == name) return var;
    for (const string &alias : var->aliases) {
      if (alias == name) return var;
    }
  }
  return nullptr;
}

Flow::Operation *Flow::Op(const string &name) {
  for (Operation *op : ops_) {
    if (op->name == name) return op;
  }
  return nullptr;
}

Flow::Function *Flow::Func(const string &name) {
  for (Function *func : funcs_) {
    if (func->name == name) return func;
  }
  return nullptr;
}

}  // namespace myelin
}  // namespace sling
