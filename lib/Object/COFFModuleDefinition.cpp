//===--- COFFModuleDefinition.cpp - Simple DEF parser ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Windows-specific.
// A parser for the module-definition file (.def file).
//
// The format of module-definition files are described in this document:
// https://msdn.microsoft.com/en-us/library/28d6s79h.aspx
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/COFFModuleDefinition.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/COFFImportFile.h"
#include "llvm/Object/Error.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm::COFF;
using namespace llvm;

namespace llvm {
namespace object {

enum Kind {
  Unknown,
  Eof,
  Identifier,
  Comma,
  Equal,
  KwBase,
  KwConstant,
  KwData,
  KwExports,
  KwHeapsize,
  KwLibrary,
  KwName,
  KwNoname,
  KwPrivate,
  KwStacksize,
  KwVersion,
};

struct Token {
  explicit Token(Kind T = Unknown, StringRef S = "") : K(T), Value(S) {}
  Kind K;
  StringRef Value;
};

static bool isDecorated(StringRef Sym) {
  return Sym.startswith("_") || Sym.startswith("@") || Sym.startswith("?");
}

static Error createError(const Twine &Err) {
  return make_error<StringError>(StringRef(Err.str()),
                                 object_error::parse_failed);
}

class Lexer {
public:
  Lexer(StringRef S) : Buf(S) {}

  Token lex() {
    Buf = Buf.trim();
    if (Buf.empty())
      return Token(Eof);

    switch (Buf[0]) {
    case '\0':
      return Token(Eof);
    case ';': {
      size_t End = Buf.find('\n');
      Buf = (End == Buf.npos) ? "" : Buf.drop_front(End);
      return lex();
    }
    case '=':
      Buf = Buf.drop_front();
      return Token(Equal, "=");
    case ',':
      Buf = Buf.drop_front();
      return Token(Comma, ",");
    case '"': {
      StringRef S;
      std::tie(S, Buf) = Buf.substr(1).split('"');
      return Token(Identifier, S);
    }
    default: {
      size_t End = Buf.find_first_of("=,\r\n \t\v");
      StringRef Word = Buf.substr(0, End);
      Kind K = llvm::StringSwitch<Kind>(Word)
                   .Case("BASE", KwBase)
                   .Case("CONSTANT", KwConstant)
                   .Case("DATA", KwData)
                   .Case("EXPORTS", KwExports)
                   .Case("HEAPSIZE", KwHeapsize)
                   .Case("LIBRARY", KwLibrary)
                   .Case("NAME", KwName)
                   .Case("NONAME", KwNoname)
                   .Case("PRIVATE", KwPrivate)
                   .Case("STACKSIZE", KwStacksize)
                   .Case("VERSION", KwVersion)
                   .Default(Identifier);
      Buf = (End == Buf.npos) ? "" : Buf.drop_front(End);
      return Token(K, Word);
    }
    }
  }

private:
  StringRef Buf;
};

class Parser {
public:
  explicit Parser(StringRef S, MachineTypes M) : Lex(S), Machine(M) {}

  Expected<COFFModuleDefinition> parse() {
    do {
      if (Error Err = parseOne())
        return std::move(Err);
    } while (Tok.K != Eof);
    return Info;
  }

private:
  void read() {
    if (Stack.empty()) {
      Tok = Lex.lex();
      return;
    }
    Tok = Stack.back();
    Stack.pop_back();
  }

  Error readAsInt(uint64_t *I) {
    read();
    if (Tok.K != Identifier || Tok.Value.getAsInteger(10, *I))
      return createError("integer expected");
    return Error::success();
  }

  Error expect(Kind Expected, StringRef Msg) {
    read();
    if (Tok.K != Expected)
      return createError(Msg);
    return Error::success();
  }

  void unget() { Stack.push_back(Tok); }

  Error parseOne() {
    read();
    switch (Tok.K) {
    case Eof:
      return Error::success();
    case KwExports:
      for (;;) {
        read();
        if (Tok.K != Identifier) {
          unget();
          return Error::success();
        }
        if (Error Err = parseExport())
          return Err;
      }
    case KwHeapsize:
      return parseNumbers(&Info.HeapReserve, &Info.HeapCommit);
    case KwStacksize:
      return parseNumbers(&Info.StackReserve, &Info.StackCommit);
    case KwLibrary:
    case KwName: {
      bool IsDll = Tok.K == KwLibrary; // Check before parseName.
      std::string Name;
      if (Error Err = parseName(&Name, &Info.ImageBase))
        return Err;
      // Append the appropriate file extension if not already present.
      StringRef Ext = IsDll ? ".dll" : ".exe";
      if (!StringRef(Name).endswith_lower(Ext))
        Name += Ext;

      // Set the output file, but don't override /out if it was already passed.
      if (Info.OutputFile.empty())
        Info.OutputFile = Name;
      return Error::success();
    }
    case KwVersion:
      return parseVersion(&Info.MajorImageVersion, &Info.MinorImageVersion);
    default:
      return createError("unknown directive: " + Tok.Value);
    }
  }

  Error parseExport() {
    COFFShortExport E;
    E.Name = Tok.Value;
    read();
    if (Tok.K == Equal) {
      read();
      if (Tok.K != Identifier)
        return createError("identifier expected, but got " + Tok.Value);
      E.ExtName = E.Name;
      E.Name = Tok.Value;
    } else {
      unget();
    }

    if (Machine == IMAGE_FILE_MACHINE_I386) {
      if (!isDecorated(E.Name))
        E.Name = (std::string("_").append(E.Name));
      if (!E.ExtName.empty() && !isDecorated(E.ExtName))
        E.ExtName = (std::string("_").append(E.ExtName));
    }

    for (;;) {
      read();
      if (Tok.K == Identifier && Tok.Value[0] == '@') {
        Tok.Value.drop_front().getAsInteger(10, E.Ordinal);
        read();
        if (Tok.K == KwNoname) {
          E.Noname = true;
        } else {
          unget();
        }
        continue;
      }
      if (Tok.K == KwData) {
        E.Data = true;
        continue;
      }
      if (Tok.K == KwConstant) {
        E.Constant = true;
        continue;
      }
      if (Tok.K == KwPrivate) {
        E.Private = true;
        continue;
      }
      unget();
      Info.Exports.push_back(E);
      return Error::success();
    }
  }

  // HEAPSIZE/STACKSIZE reserve[,commit]
  Error parseNumbers(uint64_t *Reserve, uint64_t *Commit) {
    if (Error Err = readAsInt(Reserve))
      return Err;
    read();
    if (Tok.K != Comma) {
      unget();
      Commit = nullptr;
      return Error::success();
    }
    if (Error Err = readAsInt(Commit))
      return Err;
    return Error::success();
  }

  // NAME outputPath [BASE=address]
  Error parseName(std::string *Out, uint64_t *Baseaddr) {
    read();
    if (Tok.K == Identifier) {
      *Out = Tok.Value;
    } else {
      *Out = "";
      unget();
      return Error::success();
    }
    read();
    if (Tok.K == KwBase) {
      if (Error Err = expect(Equal, "'=' expected"))
        return Err;
      if (Error Err = readAsInt(Baseaddr))
        return Err;
    } else {
      unget();
      *Baseaddr = 0;
    }
    return Error::success();
  }

  // VERSION major[.minor]
  Error parseVersion(uint32_t *Major, uint32_t *Minor) {
    read();
    if (Tok.K != Identifier)
      return createError("identifier expected, but got " + Tok.Value);
    StringRef V1, V2;
    std::tie(V1, V2) = Tok.Value.split('.');
    if (V1.getAsInteger(10, *Major))
      return createError("integer expected, but got " + Tok.Value);
    if (V2.empty())
      *Minor = 0;
    else if (V2.getAsInteger(10, *Minor))
      return createError("integer expected, but got " + Tok.Value);
    return Error::success();
  }

  Lexer Lex;
  Token Tok;
  std::vector<Token> Stack;
  MachineTypes Machine;
  COFFModuleDefinition Info;
};

Expected<COFFModuleDefinition> parseCOFFModuleDefinition(MemoryBufferRef MB,
                                                         MachineTypes Machine) {
  return Parser(MB.getBuffer(), Machine).parse();
}

} // namespace object
} // namespace llvm
