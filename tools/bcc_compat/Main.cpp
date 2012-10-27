/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include <stdlib.h>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Config/config.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/system_error.h>

#include <bcc/BCCContext.h>
#include <bcc/Compiler.h>
#include <bcc/Config/BuildInfo.h>
#include <bcc/Config/Config.h>
#include <bcc/ExecutionEngine/BCCRuntimeSymbolResolver.h>
#include <bcc/ExecutionEngine/ObjectLoader.h>
#include <bcc/ExecutionEngine/SymbolResolverProxy.h>
#include <bcc/ExecutionEngine/SymbolResolvers.h>
#include <bcc/Renderscript/RSCompilerDriver.h>
#include <bcc/Script.h>
#include <bcc/Source.h>
#include <bcc/Support/CompilerConfig.h>
#include <bcc/Support/Initialization.h>
#include <bcc/Support/InputFile.h>
#include <bcc/Support/OutputFile.h>
#include <bcc/Support/TargetCompilerConfigs.h>

using namespace bcc;

//===----------------------------------------------------------------------===//
// General Options
//===----------------------------------------------------------------------===//
namespace {

llvm::cl::list<std::string>
OptInputFilenames(llvm::cl::Positional, llvm::cl::OneOrMore,
                  llvm::cl::desc("<input bitcode files>"));

llvm::cl::opt<std::string>
OptOutputFilename("o", llvm::cl::desc("Specify the output filename"),
                  llvm::cl::value_desc("filename"));

llvm::cl::opt<std::string>
OptRuntimePath("rt-path", llvm::cl::desc("Specify the runtime library path"),
               llvm::cl::value_desc("path"));

#ifdef TARGET_BUILD
const std::string OptTargetTriple(DEFAULT_TARGET_TRIPLE_STRING);
#else
llvm::cl::opt<std::string>
OptTargetTriple("mtriple",
                llvm::cl::desc("Specify the target triple (default: "
                               DEFAULT_TARGET_TRIPLE_STRING ")"),
                llvm::cl::init(DEFAULT_TARGET_TRIPLE_STRING),
                llvm::cl::value_desc("triple"));

llvm::cl::alias OptTargetTripleC("C", llvm::cl::NotHidden,
                                 llvm::cl::desc("Alias for -mtriple"),
                                 llvm::cl::aliasopt(OptTargetTriple));
#endif

//===----------------------------------------------------------------------===//
// Compiler Options
//===----------------------------------------------------------------------===//
llvm::cl::opt<bool>
OptPIC("fPIC", llvm::cl::desc("Generate fully relocatable, position independent"
                              " code"));

llvm::cl::opt<char>
OptOptLevel("O", llvm::cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] "
                                "(default: -O2)"),
            llvm::cl::Prefix, llvm::cl::ZeroOrMore, llvm::cl::init('2'));

llvm::cl::opt<bool>
OptC("c", llvm::cl::desc("Compile and assemble, but do not link."));

//===----------------------------------------------------------------------===//
// Linker Options
//===----------------------------------------------------------------------===//
// FIXME: this option will be removed in the future when MCLinker is capable
//        of generating shared library directly from given bitcode. It only
//        takes effect when -shared is supplied.
llvm::cl::opt<std::string>
OptImmObjectOutput("or", llvm::cl::desc("Specify the filename for output the "
                                        "intermediate relocatable when linking "
                                        "the input bitcode to the shared "
                                        "library"), llvm::cl::ValueRequired);

llvm::cl::opt<bool>
OptShared("shared", llvm::cl::desc("Create a shared library from input bitcode "
                                   "files"));


// Override "bcc -version" since the LLVM version information is not correct on
// Android build.
void BCCVersionPrinter() {
  llvm::raw_ostream &os = llvm::outs();
  os << "libbcc (The Android Open Source Project, http://www.android.com/):\n"
     << "  Build time: " << BuildInfo::GetBuildTime() << "\n"
     << "  Build revision: " << BuildInfo::GetBuildRev() << "\n"
     << "  Build source blob: " << BuildInfo::GetBuildSourceBlob() << "\n"
     << "  Default target: " << DEFAULT_TARGET_TRIPLE_STRING << "\n";

  os << "\n";

  os << "LLVM (http://llvm.org/):\n"
     << "  Version: " << PACKAGE_VERSION << "\n";
  return;
}

} // end anonymous namespace

RSScript *PrepareRSScript(BCCContext &pContext,
                          const llvm::cl::list<std::string> &pBitcodeFiles) {
  RSScript *result = NULL;

  for (unsigned i = 0; i < pBitcodeFiles.size(); i++) {
    const std::string &input_bitcode = pBitcodeFiles[i];
    Source *source = Source::CreateFromFile(pContext, input_bitcode);
    if (source == NULL) {
      llvm::errs() << "Failed to load llvm module from file `" << input_bitcode
                   << "'!\n";
      return NULL;
    }

    if (result != NULL) {
      if (!result->mergeSource(*source, /* pPreserveSource */false)) {
        llvm::errs() << "Failed to merge the llvm module `" << input_bitcode
                     << "' to compile!\n";
        delete source;
        return NULL;
      }
    } else {
      result = new (std::nothrow) RSScript(*source);
      if (result == NULL) {
        llvm::errs() << "Out of memory when create script for file `"
                     << input_bitcode << "'!\n";
        delete source;
        return NULL;
      }
    }
  }

  return result;
}

static inline
bool ConfigCompiler(RSCompilerDriver &pCompilerDriver) {
  RSCompiler *compiler = pCompilerDriver.getCompiler();
  CompilerConfig *config = NULL;

#ifdef TARGET_BUILD
  config = new (std::nothrow) DefaultCompilerConfig();
#else
  config = new (std::nothrow) CompilerConfig(OptTargetTriple);
#endif
  if (config == NULL) {
    llvm::errs() << "Out of memory when create the compiler configuration!\n";
    return false;
  }

  // Setup the config according to the value of command line option.
  if (OptPIC) {
    config->setRelocationModel(llvm::Reloc::PIC_);
  }
  switch (OptOptLevel) {
    case '0': config->setOptimizationLevel(llvm::CodeGenOpt::None); break;
    case '1': config->setOptimizationLevel(llvm::CodeGenOpt::Less); break;
    case '3': config->setOptimizationLevel(llvm::CodeGenOpt::Aggressive); break;
    case '2':
    default: {
      config->setOptimizationLevel(llvm::CodeGenOpt::Default);
      break;
    }
  }

  pCompilerDriver.setConfig(config);
  Compiler::ErrorCode result = compiler->config(*config);

  if (result != Compiler::kSuccess) {
    llvm::errs() << "Failed to configure the compiler! (detail: "
                 << Compiler::GetErrorString(result) << ")\n";
    return false;
  }

  return true;
}

#define DEFAULT_OUTPUT_PATH   "/sdcard/a.out"
static inline
std::string DetermineOutputFilename(const std::string &pOutputPath) {
  if (!pOutputPath.empty()) {
    return pOutputPath;
  }

  // User doesn't specify the value to -o.
  if (OptInputFilenames.size() > 1) {
    llvm::errs() << "Use " DEFAULT_OUTPUT_PATH " for output file!\n";
    return DEFAULT_OUTPUT_PATH;
  }

  // There's only one input bitcode file.
  const std::string &input_path = OptInputFilenames[0];
  llvm::SmallString<200> output_path(input_path);

  llvm::error_code err = llvm::sys::fs::make_absolute(output_path);
  if (err != llvm::errc::success) {
    llvm::errs() << "Failed to determine the absolute path of `" << input_path
                 << "'! (detail: " << err.message() << ")\n";
    return "";
  }

  if (OptC) {
    // -c was specified. Replace the extension to .o.
    llvm::sys::path::replace_extension(output_path, "o");
  } else {
    // Use a.out under current working directory when compile executable or
    // shared library.
    llvm::sys::path::remove_filename(output_path);
    llvm::sys::path::append(output_path, "a.out");
  }

  return output_path.c_str();
}

int main(int argc, char **argv) {
  llvm::cl::SetVersionPrinter(BCCVersionPrinter);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  init::Initialize();

  if (OptRuntimePath.empty()) {
    fprintf(stderr, "You must set \"-rt-path </path/to/libclcore.bc>\" with "
                    "this tool\n");
    return EXIT_FAILURE;
  }

  BCCContext context;
  RSCompilerDriver rscd;
  Compiler compiler;

  if (!ConfigCompiler(rscd)) {
    return EXIT_FAILURE;
  }

  std::string OutputFilename = DetermineOutputFilename(OptOutputFilename);
  if (OutputFilename.empty()) {
    return EXIT_FAILURE;
  }

  RSScript *s = NULL;
  s = PrepareRSScript(context, OptInputFilenames);
  rscd.build(*s, OutputFilename.c_str(), OptRuntimePath.c_str());

  return EXIT_SUCCESS;
}
