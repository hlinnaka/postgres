//
// This program builds without issues when postgres was vonfigured with  --with-llvm flag.
//
// To run it needs a compilation database also known as compile_commands.json file. One
// way to do it is to wrap make with bear tool (https://github.com/rizsotto/Bear). For
// example:
//   bear -- make -j
//
// Another way to get compile_commands.json  is to use meson build system (TODO: test it).
//
// Once compile_commands.json is generated, run this program with the root directory of the
// postgres repository:
//   ./src/tools/pgguclifetimes/pg_static_vars ./
//


#include <cstdio>
#include <vector>
#include <format>
#include <cassert>
#include <filesystem>
#include <set>
#include <iostream>
#include <sstream>

#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>

constexpr bool PRINT_TREE_WALK = false;

static bool withUnannotated = false;
static bool skipYacc = true;

struct VisitorData
{
    int depth = 1;
    bool var_decl_subtree = false;
    std::vector<const char*> annotations;
};

static  std::set<std::string> known_vars;

// Print relevant information about each node maintaining indentation.
// Disabled by default, set PRINT_TREE_WALK to enable.
static void
debug_tree_node(CXCursor current_cursor, VisitorData* data)
{
    const CXCursorKind cursor_kind = clang_getCursorKind(current_cursor);
    const bool is_attr = clang_isAttribute(cursor_kind) != 0;
    const bool has_attr = clang_Cursor_hasAttrs(current_cursor);

    const CXString display_name = clang_getCursorDisplayName(current_cursor);
    const CXString kind_spelling = clang_getCursorKindSpelling(cursor_kind);
    const CXType cursor_type = clang_getCursorType(current_cursor);
    const CXString type_spelling = clang_getTypeSpelling(cursor_type);

    CXString loc;
    unsigned line, column;
    clang_getPresumedLocation(clang_getCursorLocation(current_cursor), &loc, &line, &column);

    std::string indent(data->depth * 2, '-');
    printf("%s: %s '%s' <%s> %s%s (%s:%u:%u, isFromMainFile=%d)\n",
        indent.c_str(),
        clang_getCString(kind_spelling),
        clang_getCString(display_name),
        clang_getCString(type_spelling),
        is_attr ? "[IS_ATTR] " : "",
        has_attr ? "[HAS_ATTR] " : "",
        clang_getCString(loc), line, column,
        clang_Location_isFromMainFile(clang_getCursorLocation(current_cursor)));

    clang_disposeString(type_spelling);
    clang_disposeString(kind_spelling);
    clang_disposeString(display_name);
}

static void
process_var(CXCursor current_cursor, std::vector<const char *> annotations)
{
    const CXType type = clang_getCursorType(current_cursor);

    const CXString var_name_cxs = clang_getCursorDisplayName(current_cursor);
    const std::string var_name = clang_getCString(var_name_cxs);
    clang_disposeString(var_name_cxs);

    const CXString type_name_cxs = clang_getTypeSpelling(type);
    std::string type_name = clang_getCString(type_name_cxs);
    clang_disposeString(type_name_cxs);

    CXString file;
    unsigned line, column;
    clang_getPresumedLocation(clang_getCursorLocation(current_cursor), &file, &line, &column);
    std::ostringstream oss;
    oss << std::filesystem::path(clang_getCString(file)).lexically_normal().string()
        << ":" << line
        << ":" << column;
    const std::string location = oss.str();

    // Skip already seen vars. We iterate over all c files, hence declarations
    // from headers might appear multiple times.
    const std::string name_loc = var_name + ":" + location;
    auto result = known_vars.insert(name_loc);
    if (!result.second) {
        return;
    }

    // skip `no_such_variable` variables which are used in BKI macroses
    if (std::string_view(var_name.data()) == "no_such_variable") {
        return;
    }

    // skip yacc files by checking "yy" in the name
    // TODO: some better heuristic
    if (skipYacc && std::string_view(var_name.data()).find("yy") != std::string::npos) {
        return;
    }

    // skip const variables
    if (clang_isConstQualifiedType(type)) {
        return;
    } else if (std::string_view(type_name.data(), 6) == "const ") {
        // somehow clang_isConstQualifiedType does not work for const arrays, check const prefix
        return;
    }

    // ok, print it

    // annotations list
    std::string annotation;
    if (!annotations.empty()) {
        annotation = annotations[0];
        for (size_t i = 1; i < annotations.size(); ++i) {
            annotation += std::string(", ") + annotations[i];
        }
    } else {
        annotation = "";
    }

    if (annotations.size() > 1) {
        fprintf(stderr, "WARNING: Multiple annotations: %s\n", annotation.c_str());
    }

    if (withUnannotated || annotation.empty() || annotations.size() > 1) {
        fprintf(stdout, "[%s]\t%s\t%s\t%s\n",
                annotation.c_str(),
                var_name.c_str(),
                type_name.c_str(),
                location.c_str());
    }
}

static CXChildVisitResult
VisitTU(CXCursor current_cursor, CXCursor parent, CXClientData client_data)
{
    VisitorData* data = reinterpret_cast<VisitorData*>(client_data);
    bool top_var_decl = false;

    VisitorData child_data;
    child_data.depth = data->depth + 1;
    child_data.var_decl_subtree = data->var_decl_subtree;

    const CXCursorKind cursor_kind = clang_getCursorKind(current_cursor);

    // Attributes are in child nodes, grab names in recursive calls
    //
    // NB: clang_Location_isFromMainFile(clang_getCursorLocation()) is not properly inherited by attribute children,
    // see https://github.com/llvm/llvm-project/issues/87813 for details.
    if (cursor_kind == CXCursor_VarDecl && clang_Location_isInSystemHeader(clang_getCursorLocation(current_cursor)) == 0) {
        child_data.var_decl_subtree = true;
        top_var_decl = true;
    }

    if (cursor_kind == CXCursor_AnnotateAttr && data->var_decl_subtree) {
        data->annotations.push_back(clang_getCString(clang_getCursorSpelling(current_cursor)));
    }

    if constexpr(PRINT_TREE_WALK) {
        debug_tree_node(current_cursor, data);
    }

    // recurse into subtree
    clang_visitChildren(current_cursor, VisitTU, &child_data);

    if (top_var_decl) {
        process_var(current_cursor, child_data.annotations );
    }

    return CXChildVisit_Continue;
};

// Function to process each translation unit (TU)
static void
processTranslationUnit(const char *filename, CXCompilationDatabase compilationDB) {
    // Arguments needed for parsing each TU, usually retrieved from the compilation database
    CXCompileCommands commands = clang_CompilationDatabase_getCompileCommands(compilationDB, filename);

    if (commands == NULL) {
        fprintf(stderr, "No compile commands found for %s\n", filename);
        return;
    }

    unsigned commandCount = clang_CompileCommands_getSize(commands);
    for (unsigned i = 0; i < commandCount; ++i) {
        CXCompileCommand command = clang_CompileCommands_getCommand(commands, i);
        CXString directory = clang_CompileCommand_getDirectory(command);
        std::string source_dir = clang_getCString(directory);
        unsigned numArgs = clang_CompileCommand_getNumArgs(command);

        std::vector<std::string> args;
        // Skip the last argument, which is the filename
        for (unsigned j = 0; j < numArgs - 1; ++j) { 
            CXString arg = clang_CompileCommand_getArg(command, j);
            std::string arg_str = clang_getCString(arg);

            // Change relative include paths to absolute paths
            if (arg_str.rfind("-I", 0) == 0 && arg_str.size() > 2) {
                std::string include_path = arg_str.substr(2);
                if (!std::filesystem::path(include_path).is_absolute()) {
                    include_path = std::filesystem::absolute(std::filesystem::path(source_dir) / include_path).string();
                    arg_str = "-I" + include_path;
                }
            }

            args.push_back(arg_str);
            clang_disposeString(arg);
        }

        // Add the additional include path
        //
        // FIXME: ideally this should be present in the compilation database. But on macOS, it
        // is not and that e.g. breaks stdbool inclusion. Which in turn make clang parse VarDecl
        // as FunctionDecl.
        args.push_back("-I/Library/Developer/CommandLineTools/usr/lib/clang/16/include/");

        // Convert the vector of strings to a const char** array for compatibility with Clang API
        std::vector<const char*> c_args;
        for (const std::string& arg : args) {
            c_args.push_back(arg.c_str());
        }

        // Parse the translation unit
        CXIndex index = clang_createIndex(0, 0);
        CXTranslationUnit translationUnit;
        auto err = clang_parseTranslationUnit2FullArgv(
            index, filename, c_args.data(), numArgs, NULL, 0,
            CXTranslationUnit_SkipFunctionBodies
                | CXTranslationUnit_VisitImplicitAttributes
		        | CXTranslationUnit_KeepGoing,
            &translationUnit);

        if (err != CXError_Success) {
            fprintf(stderr, "Failed to parse the translation unit (%d)\n", err);
            exit(1);
        }

        const int num_diags = clang_getNumDiagnostics(translationUnit);
        int num_errors = 0;
        for (int i = 0; i < num_diags; ++i)
        {
            CXDiagnostic diag = clang_getDiagnostic(translationUnit, i);
            CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diag);
            CXString s = clang_formatDiagnostic(diag, clang_defaultDiagnosticDisplayOptions());

            if (severity >= CXDiagnostic_Error) {
                num_errors++;
                fprintf(stderr, "Error: %s\n", clang_getCString(s));
            }

            clang_disposeString(s);
            clang_disposeDiagnostic(diag);
        }

        if (num_errors > 0) {
            fprintf(stderr, "Skipping '%s' due to compilation errors\n", filename);
        } else {
            // Visit each cursor in the AST
            CXCursor cursor = clang_getTranslationUnitCursor(translationUnit);
            VisitorData data;
            clang_visitChildren(cursor, VisitTU, /*user_data*/&data);
        }

        // Clean up
        clang_disposeTranslationUnit(translationUnit);
        clang_disposeIndex(index);
        clang_disposeString(directory);
    }
    clang_CompileCommands_dispose(commands);
}

int
main(int argc, char **argv) {
    std::filesystem::path compilationDBPath;
    std::vector<std::string> skipPaths;
    std::vector<std::string> defaultSkipPaths = {
        "src/backend/jit/llvm/",
        "src/bin/",
        "src/fe_utils/",
        "src/interfaces",
        "src/timezone/",
        "jit/llvm/",
        "src/test",
    };

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << "[--skip=<dir or file>...] [--with-annotated] [--no-skip-yacc] <path-to-dir-with-compile_commands.json>" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  --skip=<dir or file>      Skip processing for the specified directory or file. Can be used multiple times." << std::endl;
        std::cerr << "  --with-annotated          Show annotated variables too." << std::endl;
        std::cerr << "  --no-skip-yacc            Show variables from lexer/parsers too." << std::endl;
        return 1;
    }

    // Parse the --skip arguments
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--skip=", 7) == 0) {
            std::string skipPath = argv[i] + 7;  // Extract the path after "--skip="
            
            // If skipPath is relative, make it absolute based on compilationDBPath
            std::filesystem::path resolvedSkipPath(skipPath);
            if (resolvedSkipPath.is_relative() && !compilationDBPath.empty()) {
                skipPaths.emplace_back((compilationDBPath / resolvedSkipPath).lexically_normal().string());
            } else {
                skipPaths.emplace_back(resolvedSkipPath.lexically_normal().string());
            }
        } else if (std::string(argv[i]) == "--with-annotated") {
            withUnannotated = true;
        } else if (std::string(argv[i]) == "--no-skip-yacc") {
            skipYacc = false;
        } else {
            compilationDBPath = argv[i];  // First non-"--" argument is the compilation DB path
        }
    }

    if (compilationDBPath.empty()) {
        std::cerr << "Error: Missing path to compile_commands.json." << std::endl;
        return 1;
    }

    // Convert default skip paths to absolute paths based on compilationDBPath
    for (const std::string& path : defaultSkipPaths) {
        std::filesystem::path absPath = compilationDBPath / path;
        skipPaths.emplace_back(absPath.lexically_normal().string());
    }

    // Load the compilation database
    CXCompilationDatabase_Error error;
    CXCompilationDatabase compilationDB = clang_CompilationDatabase_fromDirectory(compilationDBPath.c_str(), &error);
    if (error != CXCompilationDatabase_NoError) {
        std::cerr << "Failed to load compile_commands.json from directory " << compilationDBPath << std::endl;
        return 1;
    }

    // Get all compile commands in the database
    CXCompileCommands allCommands = clang_CompilationDatabase_getAllCompileCommands(compilationDB);
    unsigned numCommands = clang_CompileCommands_getSize(allCommands);

    // Process each translation unit (TU) in the compilation database
    for (unsigned i = 0; i < numCommands; ++i) {
        CXCompileCommand command = clang_CompileCommands_getCommand(allCommands, i);
        CXString filename = clang_CompileCommand_getFilename(command);
        std::string filePath = clang_getCString(filename);

        // Check if filePath should be skipped
        bool skip = false;
        for (const std::string& skipPath : skipPaths) {
            if (filePath.find(skipPath) != std::string::npos) {
                skip = true;
                break;
            }
        }

        if (!skip) {
            processTranslationUnit(filePath.c_str(), compilationDB);
        }

        clang_disposeString(filename);
    }

    // Clean up
    clang_CompileCommands_dispose(allCommands);
    clang_CompilationDatabase_dispose(compilationDB);
    return 0;
}
