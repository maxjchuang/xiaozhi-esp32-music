#pragma once
class Application {
public:
    static Application& GetInstance() { static Application instance; return instance; }
    template<class F> void Schedule(F callback) { callback(); }
};
