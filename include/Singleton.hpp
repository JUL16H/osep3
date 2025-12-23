#pragma once
template <typename T>
class Singleton {
public:
    static T *get_instance() {
        static T instance;
        return &instance;
    }

    Singleton(Singleton&) = delete;
    Singleton& operator=(const Singleton&) = delete;
    Singleton(Singleton&&) = delete;
    Singleton& operator=(const Singleton&&) = delete;

protected:
    Singleton() = default;
    virtual ~Singleton() = default;
};
