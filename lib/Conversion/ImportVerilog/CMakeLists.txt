include(SlangCompilerOptions)

add_circt_translation_library(CIRCTImportVerilog
  Expressions.cpp
  FormatStrings.cpp
  HierarchicalNames.cpp
  ImportVerilog.cpp
  Statements.cpp
  Structure.cpp
  Types.cpp
  TimingControls.cpp

  DEPENDS
  slang_slang

  LINK_LIBS PUBLIC
  CIRCTDebug
  CIRCTHW
  CIRCTMoore
  MLIRFuncDialect
  MLIRSCFDialect
  MLIRTranslateLib
  PRIVATE
  slang_slang
)
