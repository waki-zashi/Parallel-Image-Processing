#include <iostream>
#include <fstream>
#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <algorithm>

using namespace std;

struct Task {
    int start_row;
    int end_row;
};

class BlockingQueue {
private:
    queue<Task> q;
    mutex mtx;
    condition_variable cv;
    bool done = false;

public:
    void push(const Task& task) {
        unique_lock<mutex> lock(mtx);
        q.push(task);
        cv.notify_one();
    }

    bool pop(Task& task) {
        unique_lock<mutex> lock(mtx);

        cv.wait(lock, [this]() {
            return !q.empty() || done;
            });

        if (q.empty() && done)
            return false;

        task = q.front();
        q.pop();
        return true;
    }

    void setDone() {
        unique_lock<mutex> lock(mtx);
        done = true;
        cv.notify_all();
    }
};

bool readPPM(const string& filename,
    int& width,
    int& height,
    vector<unsigned char>& data) {

    ifstream file(filename, ios::binary);
    if (!file) {
        cerr << "Error: Cannot open input file\n";
        return false;
    }

    string format;
    file >> format;

    if (format != "P6") {
        cerr << "Error: Unsupported format (only P6)\n";
        return false;
    }

    file >> width >> height;

    int maxVal;
    file >> maxVal;
    file.ignore(1);

    data.resize(width * height * 3);
    file.read(reinterpret_cast<char*>(data.data()), data.size());

    if (!file) {
        cerr << "Error: Failed to read image data\n";
        return false;
    }

    return true;
}

bool writePPM(const string& filename,
    int width,
    int height,
    const vector<unsigned char>& data) {

    ofstream file(filename, ios::binary);
    if (!file) {
        cerr << "Error: Cannot open output file\n";
        return false;
    }

    file << "P6\n" << width << " " << height << "\n255\n";
    file.write(reinterpret_cast<const char*>(data.data()), data.size());

    if (!file) {
        cerr << "Error: Failed to write image\n";
        return false;
    }

    return true;
}

void producer(BlockingQueue& queue,
    int height,
    int blockSize) {

    for (int i = 0; i < height; i += blockSize) {
        Task task;
        task.start_row = i;
        task.end_row = min(i + blockSize, height);
        queue.push(task);
    }

    queue.setDone();
}

void consumer(BlockingQueue& queue,
    const vector<unsigned char>& input,
    vector<unsigned char>& output,
    int width) {

    Task task;

    while (queue.pop(task)) {

        for (int y = task.start_row; y < task.end_row; y++) {
            for (int x = 0; x < width; x++) {

                int index = (y * width + x) * 3;

                output[index] = 255 - input[index];
                output[index + 1] = 255 - input[index + 1];
                output[index + 2] = 255 - input[index + 2];
            }
        }
    }
}

int main(int argc, char* argv[]) {

    if (argc != 4) {
        cout << "Usage: " << argv[0]
            << " input.ppm output.ppm num_threads\n";
        return 1;
    }

    string inputFile = argv[1];
    string outputFile = argv[2];
    int numThreads = stoi(argv[3]);

    int width, height;
    vector<unsigned char> inputImage;

    if (!readPPM(inputFile, width, height, inputImage)) {
        return 1;
    }

    vector<unsigned char> outputImage(width * height * 3);

    BlockingQueue queue;

    int blockSize = 20;

    thread prod(producer, ref(queue), height, blockSize);

    vector<thread> consumers;
    for (int i = 0; i < numThreads; i++) {
        consumers.emplace_back(consumer,
            ref(queue),
            cref(inputImage),
            ref(outputImage),
            width);
    }

    prod.join();

    for (auto& t : consumers)
        t.join();

    if (!writePPM(outputFile, width, height, outputImage)) {
        return 1;
    }

    cout << "Image processing completed successfully.\n";

    return 0;
}
