# Copyright 2023 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//xls:xls_internal"],
    licenses = ["notice"],  # Apache 2.0
)

cc_library(
    name = "extract_nodes",
    srcs = ["extract_nodes.cc"],
    hdrs = ["extract_nodes.h"],
    deps = [
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
        "//xls/codegen:block_conversion",
        "//xls/codegen:block_generator",
        "//xls/codegen:codegen_options",
        "//xls/codegen:codegen_pass",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/ir:source_location",
        "//xls/ir:type",
        "//xls/scheduling:pipeline_schedule",
        "//xls/scheduling:scheduling_options",
    ],
)

cc_test(
    name = "extract_nodes_test",
    srcs = ["extract_nodes_test.cc"],
    deps = [
        ":extract_nodes",
        "@com_google_absl//absl/container:flat_hash_set",
        "//xls/common:xls_gunit",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_matcher",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
    ],
)

cc_library(
    name = "synthesizer",
    srcs = ["synthesizer.cc"],
    hdrs = ["synthesizer.h"],
    deps = [
        ":extract_nodes",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/types:span",
        "//xls/common:thread",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/synthesis:synthesis_cc_proto",
        "//xls/synthesis/yosys:yosys_synthesis_service",
    ],
)

cc_library(
    name = "delay_manager",
    srcs = ["delay_manager.cc"],
    hdrs = ["delay_manager.h"],
    deps = [
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "//xls/common/logging",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/delay_model:delay_estimator",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:scheduling_options",
    ],
)

cc_test(
    name = "delay_manager_test",
    srcs = ["delay_manager_test.cc"],
    deps = [
        ":delay_manager",
        "@com_google_absl//absl/container:flat_hash_map",
        "//xls/common:xls_gunit",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "//xls/scheduling:scheduling_options",
    ],
)

cc_library(
    name = "node_cut",
    srcs = ["node_cut.cc"],
    hdrs = ["node_cut.h"],
    deps = [
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "//xls/common/logging",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:scheduling_options",
    ],
)

cc_test(
    name = "node_cut_test",
    srcs = ["node_cut_test.cc"],
    deps = [
        ":node_cut",
        "@com_google_absl//absl/container:flat_hash_set",
        "//xls/common:xls_gunit",
        "//xls/common:xls_gunit_main",
        "//xls/common/status:matchers",
        "//xls/ir",
        "//xls/ir:ir_parser",
        "//xls/ir:ir_test_base",
        "//xls/scheduling:scheduling_options",
    ],
)

cc_library(
    name = "iterative_sdc_scheduler",
    srcs = ["iterative_sdc_scheduler.cc"],
    hdrs = ["iterative_sdc_scheduler.h"],
    deps = [
        ":delay_manager",
        ":node_cut",
        ":synthesizer",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/types:span",
        "//xls/common/logging",
        "//xls/common/logging:log_lines",
        "//xls/common/status:ret_check",
        "//xls/common/status:status_macros",
        "//xls/ir",
        "//xls/ir:op",
        "//xls/scheduling:scheduling_options",
        "//xls/scheduling:sdc_scheduler",
        "@com_google_ortools//ortools/math_opt/cpp:math_opt",
    ],
)
