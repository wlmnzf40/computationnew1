/*
* Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 *
 */
#include "ComputeGraphBase.h"

namespace compute_graph {
    // ============================================
    // 辅助函数实现
    // ============================================
    std::string GetSourceText(const clang::Stmt* stmt, clang::ASTContext& ctx)
    {
        if (!stmt) {
            return "<null>";
        }

        clang::SourceRange range = stmt->getSourceRange();
        if (range.isInvalid()) {
            return "<invalid>";
        }

        clang::CharSourceRange charRange =
            clang::CharSourceRange::getTokenRange(range);
        std::string text = clang::Lexer::getSourceText(
            charRange, ctx.getSourceManager(), ctx.getLangOpts()).str();

        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\t', ' ');

        if (text.length() > 60) {
            text = text.substr(0, 57) + "...";
        }

        return text;
    }

    int GetSourceLine(const clang::Stmt* stmt, clang::ASTContext& ctx)
    {
        if (!stmt) {
            return 0;
        }

        clang::SourceLocation loc = stmt->getBeginLoc();
        if (loc.isInvalid()) {
            return 0;
        }

        return static_cast<int>(
            ctx.getSourceManager().getSpellingLineNumber(loc));
    }

    // 【新增】检查函数是否来自向量化intrinsic头文件
    bool IsVectorIntrinsicFunction(const clang::FunctionDecl* func,
                                           const clang::SourceManager& sm)
    {
        if (!func) return false;

        clang::SourceLocation loc = func->getLocation();
        if (loc.isInvalid()) return false;

        // 只检查系统头文件中的函数
        if (!sm.isInSystemHeader(loc)) {
            return false;
        }

        // 检查头文件名
        clang::FileID fileId = sm.getFileID(loc);
        const clang::FileEntry* fileEntry = sm.getFileEntryForID(fileId);
        if (!fileEntry) return false;

        llvm::StringRef fileName = fileEntry->getName();

        // 向量化相关头文件
        return fileName.contains("arm_neon") ||
               fileName.contains("arm_sve") ||
               fileName.contains("arm_bf16") ||
               fileName.contains("arm_fp16") ||
               fileName.contains("mmintrin") ||    // Intel MMX/SSE/AVX
               fileName.contains("immintrin") ||   // Intel AVX
               fileName.contains("avxintrin") ||
               fileName.contains("avx512");
    }
}
