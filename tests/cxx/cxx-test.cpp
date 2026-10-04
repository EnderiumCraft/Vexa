// cxx-test: C++ on Vexa (vexa-c++, libc++ built against libvexa): the
// standard containers and algorithms, strings, smart pointers, virtual
// functions and RTTI, lambdas and std::function, threads with a mutex and a
// condition variable, <chrono>, static objects' constructors and destructors.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

static int failures;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("cxx-test: FAILED: %s\n", what);
        failures++;
    }
}

// A static object: constructed before main, destroyed after it returns.
struct Global {
    int value = 0;
    Global() { value = 42; }
    ~Global() { std::printf("cxx-test: static destructor ran\n"); }
};
static Global global;

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
};
struct Square : Shape {
    double side;
    explicit Square(double s) : side(s) {}
    double area() const override { return side * side; }
};
struct Circle : Shape {
    double radius;
    explicit Circle(double r) : radius(r) {}
    double area() const override { return 3.0 * radius * radius; }
};

int main() {
    check(global.value == 42, "a static object's constructor");

    std::vector<int> v(1000);
    std::iota(v.begin(), v.end(), 1);
    std::reverse(v.begin(), v.end());
    std::sort(v.begin(), v.end());
    check(std::accumulate(v.begin(), v.end(), 0L) == 500500, "vector, sort, accumulate");

    std::string s = "Hello";
    s += ", Vexa";
    s.append(3, '!');
    check(s == "Hello, Vexa!!!" && s.find("Vexa") == 7 && std::to_string(1234) == "1234",
          "strings");

    std::map<std::string, int> m{{"one", 1}, {"two", 2}, {"three", 3}};
    std::unordered_map<int, std::string> u;
    for (int i = 0; i < 10000; i++) {
        u[i] = std::to_string(i * 2);
    }
    check(m.begin()->first == "one" && m["three"] == 3 && u.size() == 10000 && u[4999] == "9998",
          "map and unordered_map");

    std::vector<std::unique_ptr<Shape>> shapes;
    shapes.push_back(std::make_unique<Square>(2));
    shapes.push_back(std::make_unique<Circle>(1));
    double total = 0;
    for (auto &shape : shapes) {
        total += shape->area();
    }
    check(total == 7.0 && dynamic_cast<Circle *>(shapes[1].get()) &&
              !dynamic_cast<Circle *>(shapes[0].get()),
          "virtual functions and dynamic_cast");
    auto shared = std::make_shared<std::string>("shared");
    std::weak_ptr<std::string> weak = shared;
    check(shared.use_count() == 1 && weak.lock() && *weak.lock() == "shared", "shared_ptr");

    std::function<int(int)> twice = [](int x) { return 2 * x; };
    std::optional<int> maybe = twice(21);
    std::variant<int, std::string> var = std::string("text");
    check(maybe && *maybe == 42 && std::get<std::string>(var) == "text",
          "lambdas, function, optional, variant");

    // Threads: four adding under a mutex, then a condition variable.
    std::mutex lock;
    long counter = 0;
    std::atomic<int> atomic{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < 10000; i++) {
                std::lock_guard<std::mutex> guard(lock);
                counter++;
                atomic++;
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }
    check(counter == 40000 && atomic == 40000, "threads and a mutex");
    std::condition_variable ready;
    bool done = false;
    std::thread waker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> guard(lock);
        done = true;
        ready.notify_one();
    });
    auto start = std::chrono::steady_clock::now();
    {
        std::unique_lock<std::mutex> wait(lock);
        ready.wait(wait, [&] { return done; });
    }
    waker.join();
    auto waited = std::chrono::steady_clock::now() - start;
    check(done && waited >= std::chrono::milliseconds(15), "condition_variable and chrono");

    if (failures) {
        std::printf("cxx-test: %d checks failed\n", failures);
        return 1;
    }
    std::printf("cxx-test: passed\n");
    return 0;
}
