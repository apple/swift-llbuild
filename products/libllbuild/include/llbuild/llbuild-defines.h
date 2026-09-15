//===- llbuild-defines.h ------------------------------------------*- C -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// These are the C API interfaces to the llbuild library.
//
//===----------------------------------------------------------------------===//

#ifndef LLBUILD_PUBLIC_LLBUILD_DEFINES_H
#define LLBUILD_PUBLIC_LLBUILD_DEFINES_H

#if !defined(LLBUILD_PUBLIC_LLBUILD_H) && !defined(__clang_tapi__)
#error Clients must include the "llbuild.h" umbrella header.
#endif

#if defined(__cplusplus)
#define LLBUILD_EXTERN extern "C"
#else
#define LLBUILD_EXTERN extern
#endif

#if defined(__ELF__) || (defined(__APPLE__) && defined(__MACH__))
#define LLBUILD_EXPORT LLBUILD_EXTERN __attribute__((__visibility__("default")))
#else
// asume PE/COFF
#if defined(_WINDLL)
#if defined(libllbuild_EXPORTS)
#define LLBUILD_EXPORT LLBUILD_EXTERN __declspec(dllexport)
#else
#define LLBUILD_EXPORT LLBUILD_EXTERN __declspec(dllimport)
#endif
#else
#define LLBUILD_EXPORT LLBUILD_EXTERN
#endif
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#if __has_attribute(swift_name)
# define LLBUILD_SWIFT_NAME(_name) __attribute__((swift_name(#_name)))
#else
# define LLBUILD_SWIFT_NAME(_name)
#endif

#if __has_attribute(enum_extensibility)
#define LLBUILD_ENUM_ATTRIBUTES __attribute__((enum_extensibility(open)))
#else
#define LLBUILD_ENUM_ATTRIBUTES
#endif

#ifndef __has_feature
# define __has_feature(x) 0
#endif
#if !__has_feature(nullability)
# ifndef _Nullable
#  define _Nullable
# endif
# ifndef _Nonnull
#  define _Nonnull
# endif
#endif

#if __has_feature(assume_nonnull)
#define LLBUILD_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define LLBUILD_ASSUME_NONNULL_END   _Pragma("clang assume_nonnull end")
#else
#define LLBUILD_ASSUME_NONNULL_BEGIN
#define LLBUILD_ASSUME_NONNULL_END
#endif

/// A monotonically increasing indicator of the llbuild API version.
///
/// The llbuild API is *not* stable. This value allows clients to conditionally
/// compile for multiple versions of the API.
///
/// Version History:
/// 20: Added the in-memory build description APIs, including
///     `llb_buildsystem_create_with_description`.
///
/// 19: Added isResultValid API with a fallback to CAPIExternalCommand.
///
/// 18: Added support for configuring outputs of dynamic tasks via the C API.
///
/// 17: Added `llb_buildsystem_dependency_data_format_makefile_ignoring_subsequent_outputs`
///
/// 16: Added more efficient C API for working with custom tasks
///
/// 15: Added `determined_rule_needs_to_run` delegate method
///
/// 14: Added configure API to CAPIExternalCommand
///
/// 13: Update command status for custom tasks as well
///
/// 12: Invoke provideValue on ExternalCommand for all build values
///
/// 11: Added QualityOfService field to llb_buildsystem_invocation_t
///
/// 10: Changed to a llb_task_interface_t copies instead of pointers
///
/// 9: Changed the API for build keys to use bridged opaque pointers with access functions
///
/// 8: Move scheduler algorithm and lanes into llb_buildsystem_invocation_t
///
/// 7: Added destroy_context task delegate method.
///
/// 6: Added delegate methods for specific diagnostics.
///
/// 5: Added `llb_buildsystem_command_extended_result_t`, changed command_process_finished signature.
///
/// 4: Added llb_buildsystem_build_node.
///
/// 3: Added command_had_error, command_had_note and command_had_warning delegate methods.
///
/// 2: Added `llb_buildsystem_command_result_t` parameter to command_finished.
///
/// 1: Added `environment` parameter to llb_buildsystem_invocation_t.
///
/// 0: Pre-history
#define LLBUILD_C_API_VERSION 20

#endif
