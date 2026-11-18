#ifndef _RAR_FILESTR_
#define _RAR_FILESTR_

// Read text file to std::wstring
bool ReadTextFile(
  const std::wstring &Name,
  bool Config,
  bool AbortOnError,
  RAR_CHARSET SrcCharset,
  std::wstring &DataW,
  size_t MaxSize
);

// Read text file to StringList.
bool ReadTextFile(
  const std::wstring &Name,
  StringList *List,
  bool Config,
  bool AbortOnError=false,
  RAR_CHARSET SrcCharset=RCH_DEFAULT,
  bool Unquote=false,
  bool SkipComments=false,
  bool ExpandEnvStr=false
);

RAR_CHARSET DetectTextEncoding(const byte *Data,size_t DataSize);

#endif
