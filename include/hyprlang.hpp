#ifndef HYPRLANG_HPP
#define HYPRLANG_HPP

#include <string>
#include <any>
#include <vector>
#include <cstdint>
#include <hyprutils/math/Vector2D.hpp>

namespace Hyprlang {

using INT = int64_t;
using FLOAT = float;
using STRING = const char*;
using VEC2 = Hyprutils::Math::Vector2D;

class CUSTOMTYPE {
public:
    void* m_pData = nullptr;
    void* getData() { return m_pData; }
    CUSTOMTYPE() = default;
    CUSTOMTYPE(void* a, void* b, const char* c) {}
    template<typename A, typename B>
    CUSTOMTYPE(A a, B b, const char* c) {}
};
using CConfigCustomValueType = CUSTOMTYPE;

struct SConfigValue {
    std::any value;
};

class CConfigValue {
public:
    std::any value;
    bool m_bSetByUser = false;
    std::any getValue() const { return value; }
    void* const* getDataStaticPtr() const { return reinterpret_cast<void* const*>(&value); }
};

struct SHandlerOptions {
    const char* key = nullptr;
    bool handleDynamicVars = false;
};

struct SHandlerOptionsDyn {
    bool handleDynamicVars = false;
    operator SHandlerOptions() const { return SHandlerOptions{nullptr, handleDynamicVars}; }
};

struct SConfigOptions {
    bool throwAllErrors = false;
    bool allowMissingConfig = false;
};

typedef void (*PCONFIGHANDLERFUNC)(const char*, const char*);

class CParseResult {
public:
    bool error = false;
    std::string getError() const { return ""; }
    void setError(const char* err) { error = true; }
    void setError(const std::string& err) { error = true; }
};

class CConfig {
public:
    CConfig() {}
    CConfig(const char*, SConfigOptions) {}
    void addConfigValue(const char* name, const SConfigValue& val) {}
    template<typename T> void addConfigValue(const char* name, const T& val) {}
    void addSpecialCategory(const char* name, const SHandlerOptions& opts = {}) {}
    void addSpecialConfigValue(const char* cat, const char* name, const SConfigValue& val) {}
    template<typename T> void addSpecialConfigValue(const char* cat, const char* name, const T& val) {}
    void changeRootPath(const char* path) {}
    void commence() {}
    CConfigValue* getConfigValuePtr(const char* name) { static CConfigValue v; return &v; }
    CConfigValue getConfigValue(const char* name) { return CConfigValue(); }
    CConfigValue* getSpecialConfigValuePtr(const char* cat, const char* name, const char* key = nullptr) { static CConfigValue v; return &v; }
    std::vector<std::string> listKeysForSpecialCategory(const char* cat) { return {}; }
    CParseResult parse() { return CParseResult(); }
    CParseResult parseDynamic(const char* a, const char* b, const char* c = nullptr, const char* d = nullptr) { return CParseResult(); }
    CParseResult parseFile(const char*) { return CParseResult(); }
    void registerHandler(PCONFIGHANDLERFUNC fn, const char* name, const SHandlerOptionsDyn& opts) {}
    void registerHandler(PCONFIGHANDLERFUNC fn, const char* name, const SHandlerOptions& opts = {}) {}
    void removeSpecialConfigValue(const char* cat, const char* name, const char* key = nullptr) {}
    bool specialCategoryExistsForKey(const char* cat, const char* key) { return false; }
    void unregisterHandler(const char* name) {}
};

}

#endif /* HYPRLANG_HPP */
