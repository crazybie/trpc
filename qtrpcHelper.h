#pragma once
#include <QtNetwork/QtNetwork>

namespace trpc {
inline QDataStream& operator>>(QDataStream& s, std::string& v) {
  char* p;
  s >> p;
  v = p;
  delete[] p;
  return s;
}

inline QDataStream& operator<<(QDataStream& s, std::string& v) {
  return s << v.c_str();
}
}  // namespace trpc