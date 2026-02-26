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
        if (queue_.empty()) return false;
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
        static_cast<unsigned char>(0.299 * p.r + 0.587 * p.g + 0.114 * p.b);
    p.r = p.g = p.b = gray;
}

void brighten(Pixel& p, int value) {
    p.r = std::min(255, p.r + value);
    p.g = std::min(255, p.g + value);
    p.b = std::min(255, p.b + value);
}

void processBlock(std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode,
    Task task) {

    for (int y = task.start_row; y < task.end_row; ++y) {
        for (int x = 0; x < width; ++x) {
            Pixel p = input[y * width + x];

            if (mode == 1) invert(p);
            else if (mode == 2) brighten(p, 40);
            else if (mode == 3) grayscale(p);

            output[y * width + x] = p;
        }
    }
}

void consumer(BlockingQueue& queue,
    std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode) {

    Task task;
    while (queue.pop(task)) {
        processBlock(input, output, width, height, mode, task);
    }
}

long long parallelProcess(std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode,
    int num_threads) {

    BlockingQueue queue(50);
    std::vector<std::thread> workers;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i)
        workers.emplace_back(consumer,
            std::ref(queue),
            std::ref(input),
            std::ref(output),
            width,
            height,
            mode);

    const int BLOCK_SIZE = 32;
    for (int y = 0; y < height; y += BLOCK_SIZE)
        queue.push({ y, std::min(y + BLOCK_SIZE, height) });

    queue.finish();

    for (auto& t : workers)
        t.join();

    auto end = std::chrono::high_resolution_clock::now();

    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

long long singleThreadProcess(std::vector<Pixel>& input,
    std::vector<Pixel>& output,
    int width,
    int height,
    int mode) {


    auto start = std::chrono::high_resolution_clock::now();

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Pixel p = input[y * width + x];

            if (mode == 1) invert(p);
            else if (mode == 3) brighten(p, 40);
            else if (mode == 4) grayscale(p);

            output[y * width + x] = p;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();


    return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

int main() {
    std::string filename;
    std::cout << "Enter image filename (PNG/JPEG): ";
    std::cin >> filename;

    int width, height, channels;
    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 3);

    if (!data) {
        std::cout << "Failed to load image. Make sure the file exists and is a valid PNG/JPEG.\n";
        return 1;
    }

    if (width >= 7680 && height >= 4320)
        std::cout << "8K image detected\n";

    std::cout << "Loaded image: " << width << "x" << height << ", " << channels << " channels\n";

    std::vector<Pixel> input(width * height);
    std::vector<Pixel> output(width * height);

    for (int i = 0; i < width * height; ++i) {
        input[i].r = data[i * 3 + 0];
        input[i].g = data[i * 3 + 1];
        input[i].b = data[i * 3 + 2];
    }

    stbi_image_free(data);

    int mode;
    std::cout << "1 - Invert\n2 - Brighten\n3 - Grayscale\n";
    std::cout << "Select mode: ";

    while (true) {
        if (!(std::cin >> mode)) {
            std::cout << "Invalid input. Please enter a number.\n";
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            continue;
        }

        if (mode >= 1 && mode <= 3) break;

        std::cout << "Mode must be 1, 2 or 3. Try again: ";
    }

    int threads;
    unsigned int max_threads = std::thread::hardware_concurrency();
    if (max_threads == 0) max_threads = 8; // fallback

    std::cout << "Enter number of threads (1 - " << max_threads << "): ";

    while (true) {
        if (!(std::cin >> threads)) {
            std::cout << "Invalid input. Please enter a number.\n";
            std::cin.clear();
            std::cin.ignore(10000, '\n');
            continue;
        }

        if (threads >= 1 && threads <= (int)max_threads)
            break;

        std::cout << "Threads must be between 1 and "
            << max_threads << ". Try again: ";
    }

    long long single_time =
        singleThreadProcess(input, output, width, height, mode);

    long long parallel_time =
        parallelProcess(input, output, width, height, mode, threads);

    std::cout << "\nSingle-thread time: " << single_time << " ms\n";
    std::cout << "Parallel time: " << parallel_time << " ms\n";

    if (parallel_time > 0)
        std::cout << "Speedup: "
        << (double)single_time / parallel_time
        << "x\n";

    std::string output_filename;
    std::cout << "\nEnter output filename (PNG recommended): ";
    std::cin >> output_filename;

    if (stbi_write_png(output_filename.c_str(), width, height, 3,
        output.data(), width * 3)) {
        std::cout << "Image saved successfully as PNG\n";
    }
    else {
        std::cout << "Failed to save image\n";
    }

    std::cout << "Done\n";
    return 0;
}
