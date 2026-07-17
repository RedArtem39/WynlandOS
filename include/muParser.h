#pragma once
#include <string>

namespace mu {

class Parser {
public:
    class exception_type {
    public:
        std::string GetMsg() const { return "error"; }
    };

    void DefineConst(const std::string& name, double val) {}
    void SetExpr(const std::string& expr) {}
    double Eval() { return 0.0; }
};

} // namespace mu
