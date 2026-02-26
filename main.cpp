#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <iostream>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <string>
#include <stdexcept>
#include <algorithm>

struct Pixel {
    unsigned char r, g, b;
};

struct Task {
    int start_row;
    int end_row;
};

class BlockingQueue {
private:
    std::queue<Task> queue_;
    std::mutex mutex_;
    std::condition_variable not_empty_;
    std::condition_variable not_full_;
    size_t max_size_;
    bool finished_ = false;

public:
    BlockingQueue(size_t max_size) : max_size_(max_size) {}

    bool push(Task task) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_full_.wait(lock, [&] {
            return queue_.size() < max_size_ || finished_;
            });

        if (finished_) return false;

        queue_.push(task);
        not_empty_.notify_one();
        return true;
    }

    bool pop(Task& task) {
        std::unique_lock<std::mutex> lock(mutex_);

        not_empty_.wait(lock, [&] {
            return !queue_.empty() || finished_;
            });

        if (queue_.empty())
            return false;

        task = queue_.front();
        queue_.pop();
        not_full_.notify_one();
        return true;
    }

    void finish() {
        std::lock_guard<std::mutex> lock(mutex_);
        finished_ = true;
        not_empty_.notify_all();
        not_full_.notify_all();
    }
};

void invert(Pixel& p) {
    p.r = 255 - p.r;
    p.g = 255 - p.g;
    p.b = 255 - p.b;
}

void grayscale(Pixel& p) {
    unsigned char gray =
        static_cast<unsigned char>(
            0.299 * p.r +
            0.587 * p.g +
            0.114 * p.b);
    p.r = p.g = p.b = gray;
}

void brighten(Pixel& p, int value) {
    p.r = static_cast<unsigned char>(
        std::clamp(int(p.r) + value, 0, 255));
    p.g = static_cast<unsigned char>(
        std::clamp(int(p.g) + value, 0, 255));
    p.b = static_cast<unsigned char>(
        std::clamp(int(p.b) + value, 0, 255));
}

void processBlock(std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode,
    Task task)
{
    if (task.start_row<0 || task.end_row>height)
        throw std::runtime_error("Invalid task range");

    for (int y = task.start_row; y < task.end_row; ++y)
        for (int x = 0; x < width; ++x) {

            Pixel p = input[y * width + x];

            if (mode == 1) invert(p);
            else if (mode == 2) brighten(p, 40);
            else if (mode == 3) grayscale(p);
            else throw std::runtime_error("Unknown mode");

            output[y * width + x] = p;
        }
}

void consumer(BlockingQueue& queue,
    std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode,
    std::atomic<bool>& error_flag)
{
    try {
        Task task;
        while (!error_flag && queue.pop(task))
            processBlock(input, output, width, height, mode, task);
    }
    catch (...) {
        error_flag = true;
        queue.finish();
    }
}

long long parallelProcess(std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode,
    int num_threads)
{
    if (num_threads <= 0)
        throw std::runtime_error("Invalid thread count");

    BlockingQueue queue(50);
    std::vector<std::thread> workers;
    std::atomic<bool> error_flag(false);

    auto start =
        std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
        workers.emplace_back(
            consumer,
            std::ref(queue),
            std::ref(input),
            std::ref(output),
            width,
            height,
            mode,
            std::ref(error_flag));

    const int BLOCK_SIZE = 32;


    for (int y = 0; y < height; y += BLOCK_SIZE)
        if (!queue.push(
            { y,std::min(y + BLOCK_SIZE,height) }))
            throw std::runtime_error("Queue push failed");


    queue.finish();

    for (auto& t : workers)
        if (t.joinable())
            t.join();

    if (error_flag)
        throw std::runtime_error("Worker thread failure");

    auto end =
        std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast<
        std::chrono::milliseconds>(end - start).count();
}

long long singleThreadProcess(
    std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode)
{
    auto start =
        std::chrono::high_resolution_clock::now();

    processBlock(input, output, width, height,
        mode, { 0,height });

    auto end =
        std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast<
        std::chrono::milliseconds>(end - start).count();
}

int main() {
    try {

        std::string filename;
        std::cout << "Enter image filename: ";
        std::cin >> filename;

        if (filename.empty())
            throw std::runtime_error("Empty filename");

        int width, height, channels;

        unsigned char* data =
            stbi_load(filename.c_str(),
                &width, &height, &channels, 3);

        if (!data)
            throw std::runtime_error("Image load failed");

        if (width <= 0 || height <= 0)
            throw std::runtime_error("Invalid image size");

        std::vector<Pixel> input(width * height);
        std::vector<Pixel> output(width * height);

        for (int i = 0; i < width * height; ++i) {
            input[i].r = data[i * 3];
            input[i].g = data[i * 3 + 1];
            input[i].b = data[i * 3 + 2];
        }

        stbi_image_free(data);

        int mode;

        while (true) {
            std::cout << "\nSelect mode:\n";
            std::cout << "1 - Invert\n";
            std::cout << "2 - Brighten\n";
            std::cout << "3 - Grayscale\n";
            std::cout << "Enter choice (1-3): ";

            if (!(std::cin >> mode)) {
                std::cout << "Invalid input! Please enter an integer.\n";
                std::cin.clear();
                std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
                continue;
            }

            if (mode == 1 || mode == 2 || mode == 3)
                break;

            std::cout << "Error: mode must be strictly 1, 2, or 3.\n";
        }

        int threads;
        std::cout << "Threads: ";
        std::cin >> threads;

        long long single_time =
            singleThreadProcess(
                input, output, width, height, mode);

        long long parallel_time =
            parallelProcess(
                input, output, width, height,
                mode, threads);

        std::cout << "\nSingle: "
            << single_time << " ms\n";
        std::cout << "Parallel: "
            << parallel_time << " ms\n";

        if (parallel_time > 0)
            std::cout << "Speedup: "
            << (double)single_time /
            parallel_time << "x\n";

        std::string out;
        std::cout << "Output filename: ";
        std::cin >> out;

        if (!stbi_write_png(
            out.c_str(),
            width, height, 3,
            output.data(),
            width * 3))
            throw std::runtime_error("Save failed");

    }
    catch (const std::exception& e) {
        std::cerr << "Error: "
            << e.what() << std::endl;
        return 1;
    }
    catch (...) {
        std::cerr << "Unknown error\n";
        return 1;
    }

    return 0;
}
