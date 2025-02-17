/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/dtensor/mlir/dtensor_send_recv.h"

#include <string>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_device.h"
#include "tensorflow/compiler/mlir/tensorflow/ir/tf_ops.h"
#include "tensorflow/compiler/mlir/tensorflow/transforms/collection_ops_util.h"
#include "tensorflow/compiler/mlir/tensorflow/utils/convert_tensor.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/dtensor/cc/constants.h"
#include "tensorflow/dtensor/mlir/device_utils.h"
#include "tensorflow/dtensor/mlir/layout_parsing.h"
#include "tensorflow/dtensor/mlir/op_utils.h"
#include "tensorflow/dtensor/mlir/spmd_expander_common.h"
#include "tensorflow/dtensor/mlir/value_utils.h"

namespace tensorflow {
namespace dtensor {
namespace {

// Returns compilation key placeholder. This placeholder will be replaced with
// output of TPUCompile op during TPURewrite pass. Program key (output of
// TPUCompile op) is used to differentiate TPU computation from which to receive
// data.
mlir::Value GetOrCreateCompilationKey(mlir::Operation* op) {
  mlir::Value key;
  auto cluster = op->getParentOfType<mlir::tf_device::ClusterOp>();
  assert(cluster);
  cluster.walk(
      [&](mlir::TF::_TPUCompileMlirPlaceholderProgramKeyOp compilation_key) {
        key = compilation_key.program();
      });
  if (key) return key;

  mlir::OpBuilder builder(&cluster.GetBody().front());
  auto result_type =
      mlir::RankedTensorType::get({3}, builder.getType<mlir::TF::StringType>());
  auto new_compilation_key =
      builder.create<mlir::TF::_TPUCompileMlirPlaceholderProgramKeyOp>(
          cluster.getLoc(), /*program=*/result_type,
          llvm::ArrayRef<mlir::Value>{});
  return new_compilation_key.program();
}

}  // namespace

StatusOr<mlir::Value> GetDeviceOrdinal(const Mesh& mesh,
                                       const mlir::Location& loc,
                                       mlir::func::FuncOp function,
                                       mlir::OpBuilder* builder,
                                       bool return_int64_type) {
  // Create as many entries as the number of devices in the entire mesh.
  llvm::SmallVector<int32, 4> device_id_to_ordinal(mesh.num_devices(), 0);
  // Only fill in entries with indices equal to local device IDs. For TPUs,
  // there are usually 8 local devices.
  for (int i = 0; i < mesh.local_device_ids().size(); ++i) {
    device_id_to_ordinal[mesh.local_device_ids()[i]] = i;
  }
  // Slice out the device ordinal using the device ID as index.
  TF_ASSIGN_OR_RETURN(mlir::Value device_id, DeviceId(function));
  mlir::TF::SliceOp device_ordinal = builder->create<mlir::TF::SliceOp>(
      loc,
      /*output=*/EffectivelyScalarR1Type(builder->getIntegerType(32)),
      /*input=*/IntConst(*builder, loc, device_id_to_ordinal),
      /*begin=*/
      mlir::TF::collection_ops_util::ReshapeScalarToSizeType(*builder,
                                                             device_id, loc),
      /*size=*/IntConst(*builder, loc, {1}));
  mlir::Value device_ordinal_scalar =
      ReshapeSizeTypeToScalar(*builder, loc, device_ordinal);
  if (return_int64_type) {
    device_ordinal_scalar = builder->create<mlir::TF::CastOp>(
        loc, mlir::RankedTensorType::get({}, builder->getI64Type()),
        device_ordinal_scalar);
  }
  return device_ordinal_scalar;
}

StatusOr<mlir::Operation*> LowerDTensorSendToTFOp(
    const Layout& send_input_layout, mlir::Value send_input,
    mlir::TF::DTensorSend dtensor_send) {
  mlir::OpBuilder builder(dtensor_send);
  builder.setInsertionPointAfter(send_input.getDefiningOp());
  std::string tensor_name = dtensor_send.key().str();

  Layout target_layout = dtensor_send.target_layout();
  absl::Span<const std::string> sending_devices =
      send_input_layout.mesh().local_devices();
  absl::Span<const std::string> receiving_devices =
      target_layout.mesh().local_devices();

  mlir::Operation* lowered_send_op;
  lowered_send_op = builder.create<mlir::TF::_HostSendOp>(
      send_input.getLoc(), send_input, tensor_name, sending_devices[0],
      /*send_device_incarnation=*/0, receiving_devices[0],
      /*client_terminated=*/false);

  dtensor_send.erase();
  return lowered_send_op;
}

// Lowers DTensorSend Op to either one of XlaSendFromHost op or XlaSendToHost,
// depending on the src mesh cluster.
StatusOr<mlir::Operation*> LowerDTensorSendToXlaOp(
    const Layout& send_input_layout, mlir::Value send_input,
    mlir::TF::DTensorSend dtensor_send, bool send_from_device_zero) {
  const bool send_from_cpu = !send_input_layout.mesh().is_tpu_mesh();
  mlir::OpBuilder builder(dtensor_send);

  mlir::Location loc = dtensor_send.getLoc();
  mlir::Operation* lowered_send_op;
  if (send_from_cpu) {
    llvm::SmallVector<mlir::Value, 4> value_to_send{send_input};
    mlir::OpBuilder::InsertPoint insertion_point = builder.saveInsertionPoint();
    mlir::Value program_key = GetOrCreateCompilationKey(dtensor_send);
    builder.restoreInsertionPoint(insertion_point);

    mlir::Value device_ordinal;
    if (send_from_device_zero) {
      // For CopyToMesh, we currently only support sending from host device 0
      // to target TPUs.
      device_ordinal = CreateIntScalarConst(0, builder, loc);
    } else {
      // For special topologies, always send from CPU device i to TPU device i.
      auto send_cluster =
          dtensor_send->getParentOfType<mlir::tf_device::ClusterOp>();
      if (!send_cluster) {
        return errors::InvalidArgument("DTensorSend is not inside a ClusterOp");
      }
      auto send_func = send_cluster->getParentOfType<mlir::func::FuncOp>();
      if (!send_func) {
        return errors::InvalidArgument("DTensorSend is not inside a FuncOp");
      }
      TF_ASSIGN_OR_RETURN(
          device_ordinal,
          GetDeviceOrdinal(send_input_layout.mesh(), loc, send_func, &builder));
    }
    // Create XlaSendFromHostV2 op
    lowered_send_op = builder.create<mlir::TF::_XlaSendFromHostV2Op>(
        loc, value_to_send, program_key, device_ordinal, dtensor_send.key());
  } else {
    // Note that for ops running in XLA/TPU, device ordinal input is not needed.
    lowered_send_op = builder.create<mlir::TF::XlaSendToHostOp>(
        loc, send_input, dtensor_send.key());
  }

  dtensor_send.erase();
  return lowered_send_op;
}

// Lowers DTensorRecv op to either one of XlaRecvAtHost or XlaRecvFromHost,
// depending on src mesh cluster configuration.
StatusOr<mlir::Operation*> LowerDTensorRecvToXlaOp(
    mlir::TF::DTensorRecv dtensor_recv) {
  return LowerDTensorRecvToXlaOp(dtensor_recv, dtensor_recv.getType());
}

StatusOr<mlir::Operation*> LowerDTensorRecvToXlaOp(
    const Mesh&, mlir::TF::DTensorRecv dtensor_recv, mlir::Type output_type) {
  return LowerDTensorRecvToXlaOp(dtensor_recv, output_type);
}

// Lowers DTensorRecv op to either one of XlaRecvAtHost or XlaRecvFromHost,
// depending on src mesh cluster configuration. `output_type` can be set to the
// specific local tensor type needed, if different from the Recv op output type.
StatusOr<mlir::Operation*> LowerDTensorRecvToXlaOp(
    mlir::TF::DTensorRecv dtensor_recv, mlir::Type output_type) {
  const bool recv_at_cpu = dtensor_recv.layout().mesh().is_cpu_mesh();
  mlir::Operation* recv_xla_op = nullptr;
  mlir::OpBuilder builder(dtensor_recv);

  if (recv_at_cpu) {
    // Create XlaRecvAtHostV2 op.
    llvm::SmallVector<mlir::Type, 4> output_types{output_type};
    auto recv_cluster =
        dtensor_recv->getParentOfType<mlir::tf_device::ClusterOp>();

    TF_ASSIGN_OR_RETURN(absl::optional<Mesh> mesh,
                        ExtractDeviceMeshFromOp(recv_cluster));
    if (!mesh.has_value())
      return errors::InvalidArgument(
          "failed to get device ordinal as mesh for operation is not "
          "specified.");

    mlir::OpBuilder builder(&recv_cluster.GetBody().front());
    TF_ASSIGN_OR_RETURN(
        mlir::Value device_ordinal,
        GetDeviceOrdinal(*mesh, recv_cluster.getLoc(),
                         recv_cluster->getParentOfType<mlir::func::FuncOp>(),
                         &builder));

    auto program_key = GetOrCreateCompilationKey(dtensor_recv);
    builder.setInsertionPoint(dtensor_recv);
    recv_xla_op = builder.create<mlir::TF::_XlaRecvAtHostV2Op>(
        dtensor_recv.getLoc(), output_types,
        /*dynamic_key=*/program_key, device_ordinal, dtensor_recv.keyAttr());
  } else {
    // Create XlaRecvFromHost op.
    recv_xla_op = builder.create<mlir::TF::XlaRecvFromHostOp>(
        dtensor_recv.getLoc(), output_type,
        ConvertTypeToTensorShapeAttr(dtensor_recv.getType()),
        dtensor_recv.keyAttr());
  }

  assert(recv_xla_op);

  // TODO(hongjunchoi): After receiving tensor, convert tensor to requested
  // layout with EmitRelayout.
  return recv_xla_op;
}

// Lowers a DTensorSend Op from a CPU to a TF Send op.
StatusOr<mlir::Operation*> LowerDTensorSendFromCPUToTFOp(
    const Layout& send_input_layout, mlir::Value send_input,
    mlir::TF::DTensorSend dtensor_send) {
  mlir::OpBuilder builder(dtensor_send);
  builder.setInsertionPointAfter(send_input.getDefiningOp());

  llvm::SmallVector<mlir::Value, 4> value_to_send{send_input};

  // Create multiple send from host. There should be #number of local
  // devices(in target mesh) number of sends.
  absl::Span<const std::string> sending_devices =
      send_input_layout.mesh().local_devices();

  Layout target_layout = dtensor_send.target_layout();
  absl::Span<const std::string> receiving_devices =
      target_layout.mesh().local_devices();

  std::string tensor_name = dtensor_send.key().str();

  mlir::Operation* lowered_send_op;
  for (size_t i = 0; i < receiving_devices.size(); ++i)
    lowered_send_op = builder.create<mlir::TF::_HostSendOp>(
        send_input.getLoc(), dtensor_send.input(), tensor_name,
        sending_devices[0],
        /*send_device_incarnation=*/0, receiving_devices[i]);

  dtensor_send.erase();
  return lowered_send_op;
}

// Lowers DTensorRecv op to TF Recv Op.
StatusOr<mlir::Operation*> LowerDTensorRecvFromCPUToTFOp(
    const Mesh& send_mesh, mlir::TF::DTensorRecv dtensor_recv) {
  const Layout& recv_layout = dtensor_recv.layout();

  auto recv_cluster =
      dtensor_recv->getParentOfType<mlir::tf_device::ClusterOp>();

  mlir::OpBuilder builder(&recv_cluster.GetBody().front());
  llvm::SmallVector<mlir::Type, 4> output_types{dtensor_recv.getType()};
  builder.setInsertionPoint(dtensor_recv);
  std::string tensor_name = dtensor_recv.key().str();
  absl::Span<const std::string> sending_devices = send_mesh.local_devices();
  absl::Span<const std::string> receiving_devices =
      recv_layout.mesh().local_devices();

  mlir::Operation* lowered_recv_op;
  mlir::Location loc = dtensor_recv.getLoc();
  for (size_t i = 0; i < receiving_devices.size(); ++i)
    lowered_recv_op = builder.create<mlir::TF::_HostRecvOp>(
        loc, dtensor_recv.getType(), tensor_name, sending_devices[0],
        /*send_device_incarnation=*/0, receiving_devices[i]);

  // Replace dtensor_recv with newly created recv op and remove DTensorRecv op.
  assert(lowered_recv_op);
  dtensor_recv.replaceAllUsesWith(lowered_recv_op);
  dtensor_recv.erase();
  return lowered_recv_op;
}

StatusOr<mlir::Operation*> LowerDTensorRecvToTFOp(
    const Mesh& send_mesh, mlir::TF::DTensorRecv dtensor_recv,
    mlir::Type output_type) {
  const Layout& recv_layout = dtensor_recv.layout();
  auto recv_cluster =
      dtensor_recv->getParentOfType<mlir::tf_device::ClusterOp>();

  mlir::OpBuilder builder(&recv_cluster.GetBody().front());
  builder.setInsertionPoint(dtensor_recv);
  std::string tensor_name = dtensor_recv.key().str();
  absl::Span<const std::string> sending_devices = send_mesh.local_devices();
  absl::Span<const std::string> receiving_devices =
      recv_layout.mesh().local_devices();

  mlir::Location loc = dtensor_recv.getLoc();
  mlir::Operation* lowered_recv_op = builder.create<mlir::TF::_HostRecvOp>(
      loc, output_type, tensor_name, sending_devices[0],
      /*send_device_incarnation=*/0, receiving_devices[0]);

  return lowered_recv_op;
}

namespace {
template <typename It, typename Fn>
llvm::SmallVector<mlir::Attribute, 4> GenerateBranches(
    mlir::Operation* op, mlir::SymbolTable& symbol_table,
    llvm::ArrayRef<mlir::Type> result_types, const char* fmt, const It& values,
    const Fn& fn) {
  llvm::SmallVector<mlir::Attribute, 4> branches;
  for (const auto& it : llvm::enumerate(values)) {
    // Builds the restore op on device_id.
    mlir::OpBuilder builder(op);
    auto func_type = mlir::FunctionType::get(
        builder.getContext(), op->getOperandTypes(), result_types);

    mlir::Location location = op->getLoc();
    mlir::func::FuncOp func_op = mlir::func::FuncOp::create(
        location, llvm::formatv(fmt, OpName(op), OpHash(op), it.index()).str(),
        func_type, llvm::ArrayRef<mlir::NamedAttribute>{});

    func_op.setVisibility(mlir::SymbolTable::Visibility::Private);
    symbol_table.insert(func_op);

    mlir::Block* fn_block = func_op.addEntryBlock();
    mlir::OpBuilder fn_builder = mlir::OpBuilder::atBlockBegin(fn_block);
    mlir::BlockArgument arg = (func_op.getNumArguments() > 0)
                                  ? func_op.getArgument(0)
                                  : mlir::BlockArgument{};
    auto branch_op = fn(fn_builder, location, arg, it.value());
    fn_builder.create<mlir::func::ReturnOp>(location, branch_op->getResults());

    branches.push_back(mlir::SymbolRefAttr::get(func_op));
  }
  return branches;
}
}  // namespace

StatusOr<mlir::Operation*> LowerOneToOneDTensorSendToTFHostSend(
    const Layout& send_layout, const Mesh& recv_mesh,
    mlir::TF::DTensorSend dtensor_send) {
  const auto& send_mesh = send_layout.mesh();
  bool i32_copy = dtensor_send.input().getType().getElementType().isInteger(32);
  auto module = dtensor_send->getParentOfType<mlir::ModuleOp>();
  mlir::SymbolTable symbol_table(module);
  auto device_pairs =
      llvm::zip(send_mesh.local_devices(), recv_mesh.local_devices());
  mlir::OpBuilder builder(dtensor_send);

  auto send_cluster =
      dtensor_send->getParentOfType<mlir::tf_device::ClusterOp>();
  auto send_fn = send_cluster->getParentOfType<mlir::func::FuncOp>();
  TF_ASSIGN_OR_RETURN(std::optional<Mesh> mesh,
                      ExtractDeviceMeshFromOp(send_cluster));
  TF_ASSIGN_OR_RETURN(
      mlir::Value device_ordinal,
      GetDeviceOrdinal(*mesh, dtensor_send.getLoc(), send_fn, &builder,
                       /*return_int64_type=*/false));

  mlir::StringAttr tensor_name =
      builder.getStringAttr(dtensor_send.key().str());
  auto branches = GenerateBranches(
      dtensor_send, symbol_table, llvm::ArrayRef<mlir::Type>{},
      "{0}_send_{1}_{2}", device_pairs,
      [&](mlir::OpBuilder& op_builder, auto& loc, mlir::BlockArgument arg,
          auto device_pair) -> mlir::Operation* {
        auto func_op = mlir::dyn_cast_or_null<mlir::func::FuncOp>(
            arg.getOwner()->getParentOp());
        func_op.setArgAttr(arg.getArgNumber(), kCustomDeviceAttr,
                           op_builder.getStringAttr(send_layout.ToString()));
        mlir::Value val = arg;
        if (i32_copy) {
          auto val_type = val.getType().cast<mlir::TensorType>();
          val = op_builder
                    .create<mlir::TF::CastOp>(
                        loc,
                        mlir::RankedTensorType::get(
                            val_type.getShape(), op_builder.getIntegerType(64)),
                        val)
                    ->getResult(0);
        }
        return op_builder.create<mlir::TF::_HostSendOp>(
            loc, val, tensor_name, std::get<0>(device_pair),
            /*send_device_incarnation=*/0, std::get<1>(device_pair));
      });
  mlir::Operation* case_op = builder.create<mlir::TF::CaseOp>(
      dtensor_send.getLoc(),
      /*output=*/llvm::ArrayRef<mlir::Type>{},
      /*branch_index=*/device_ordinal,
      /*input=*/dtensor_send->getOperands(),
      /*branches=*/builder.getArrayAttr(branches),
      /*is_stateless=*/builder.getBoolAttr(false));

  // erase the send op here iff targeting a gpu
  // otherwise there will be 'op not within cluster' error(s)
  if (recv_mesh.device_type() == "GPU") {
    dtensor_send.erase();
  }

  return case_op;
}

StatusOr<mlir::Operation*> LowerOneToOneDTensorRecvToTFHostRecv(
    const Mesh& send_mesh, const Layout& recv_layout,
    mlir::TF::DTensorRecv dtensor_recv) {
  auto module = dtensor_recv->getParentOfType<mlir::ModuleOp>();
  const auto& recv_mesh = recv_layout.mesh();
  mlir::SymbolTable symbol_table(module);
  auto device_pairs =
      llvm::zip(send_mesh.local_devices(), recv_mesh.local_devices());
  mlir::OpBuilder builder(dtensor_recv);

  auto recv_cluster =
      dtensor_recv->getParentOfType<mlir::tf_device::ClusterOp>();
  auto recv_fn = recv_cluster->getParentOfType<mlir::func::FuncOp>();
  TF_ASSIGN_OR_RETURN(std::optional<Mesh> mesh,
                      ExtractDeviceMeshFromOp(recv_cluster));
  TF_ASSIGN_OR_RETURN(
      mlir::Value device_ordinal,
      GetDeviceOrdinal(*mesh, recv_cluster.getLoc(), recv_fn, &builder,
                       /*return_int64_type=*/false));

  mlir::TensorType recv_type = dtensor_recv.getType();
  bool i32_copy = recv_type.getElementType().isInteger(32);
  TF_ASSIGN_OR_RETURN(
      mlir::TensorType local_recv_type,
      LocalTypeFromGlobalType(dtensor_recv.layout(), recv_type));
  mlir::TensorType local_output_type =
      i32_copy ? mlir::RankedTensorType::get(local_recv_type.getShape(),
                                             builder.getIntegerType(64))
               : local_recv_type;

  mlir::StringAttr tensor_name =
      builder.getStringAttr(dtensor_recv.key().str());
  auto branches = GenerateBranches(
      dtensor_recv, symbol_table, llvm::ArrayRef<mlir::Type>{local_output_type},
      "{0}_receive_{1}_{2}", device_pairs,
      [&](mlir::OpBuilder& op_builder, auto& loc, auto _,
          auto device_pair) -> mlir::Operation* {
        auto recv_op = op_builder.create<mlir::TF::_HostRecvOp>(
            loc, local_output_type, tensor_name, std::get<0>(device_pair),
            /*send_device_incarnation=*/0, std::get<1>(device_pair));
        SetSingleLayoutOnOp(recv_op, recv_layout);
        return recv_op;
      });
  mlir::Operation* case_op = builder.create<mlir::TF::CaseOp>(
      dtensor_recv.getLoc(),
      /*output=*/llvm::ArrayRef<mlir::Type>{local_output_type},
      /*branch_index=*/device_ordinal,
      /*input=*/dtensor_recv->getOperands(),
      /*branches=*/builder.getArrayAttr(branches),
      /*is_stateless=*/builder.getBoolAttr(false));

  mlir::Operation* lowered_recv;
  if (i32_copy) {
    lowered_recv = builder.create<mlir::TF::CastOp>(
        dtensor_recv.getLoc(), local_recv_type, case_op->getResult(0));
  } else {
    lowered_recv = case_op;
  }

  dtensor_recv.output().replaceAllUsesWith(lowered_recv->getResult(0));
  dtensor_recv.erase();

  return lowered_recv;
}

}  // namespace dtensor
}  // namespace tensorflow
