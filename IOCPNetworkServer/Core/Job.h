#pragma once
#include <functional>

class Job
{
public:
    explicit Job(std::function<void()> callback) : _callback(std::move(callback)) {} //void () 형태의 함수를 받음 

    void Execute() { _callback(); } //실행시킴 

private:
    std::function<void()> _callback;
};
