// Copyright 2026 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "xls/codegen_v_1_5/scheduling_pass.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/status/status_macros.h"
#include "xls/estimators/delay_model/delay_estimators.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/package.h"
#include "xls/scheduling/scheduling_options.h"
#include "xls/tools/schedule.h"

ABSL_FLAG(int64_t, stages, 3, "Number of stages in each proc.");
ABSL_FLAG(int64_t, ii, 1, "Initiation interval allowed by the scheduler.");
ABSL_FLAG(int64_t, clock_period_ps, 0, "Optional scheduling clock period.");
ABSL_FLAG(std::string, delay_model, "unit", "XLS delay estimator.");
ABSL_FLAG(std::string, schedule_out, "", "Optional schedule textproto output.");

namespace xls::stage_gfsim {
namespace {

absl::Status ScheduleIr(std::string_view input, std::string_view output) {
  XLS_ASSIGN_OR_RETURN(std::string text, GetFileContents(input));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> package,
                       Parser::ParsePackage(text, input));
  XLS_RETURN_IF_ERROR(package->GetTopAsProc().status());
  XLS_ASSIGN_OR_RETURN(const DelayEstimator* estimator,
                       GetDelayEstimator(absl::GetFlag(FLAGS_delay_model)));
  const int64_t stages = absl::GetFlag(FLAGS_stages);
  const int64_t ii = absl::GetFlag(FLAGS_ii);
  const int64_t period = absl::GetFlag(FLAGS_clock_period_ps);
  if (stages < 1 || ii < 1 || period < 0) {
    return absl::InvalidArgumentError(
        "Invalid stage count, II, or clock period");
  }
  SchedulingOptions options;
  options.pipeline_stages(stages).worst_case_throughput(ii).schedule_all_procs(
      true);
  if (period > 0) {
    options.clock_period_ps(period);
  }
  XLS_ASSIGN_OR_RETURN(SchedulingResult scheduled,
                       Schedule(package.get(), options, estimator));
  codegen::BlockConversionPassOptions pass_options;
  pass_options.package_schedule = scheduled.package_schedule;
  pass_options.delay_estimator = estimator;
  PassResults results;
  codegen::BlockConversionContext context;
  XLS_RETURN_IF_ERROR(codegen::SchedulingPass()
                          .Run(package.get(), pass_options, &results, context)
                          .status());
  std::string schedule_out = absl::GetFlag(FLAGS_schedule_out);
  if (!schedule_out.empty()) {
    XLS_RETURN_IF_ERROR(
        SetTextProtoFile(schedule_out, scheduled.package_schedule));
  }
  return SetFileContents(output, package->DumpIr());
}

}  // namespace
}  // namespace xls::stage_gfsim

int main(int argc, char** argv) {
  constexpr std::string_view kUsage =
      "schedule_proc [--stages=N] [--ii=N] input.ir scheduled.ir";
  auto args = xls::InitXls(kUsage, argc, argv);
  if (args.size() != 2) {
    return xls::ExitStatus(absl::InvalidArgumentError(kUsage));
  }
  return xls::ExitStatus(xls::stage_gfsim::ScheduleIr(args[0], args[1]));
}
