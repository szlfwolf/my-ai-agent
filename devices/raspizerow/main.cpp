#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <iostream>
#include <alsa/asoundlib.h>
#include <mpg123.h>
#include <curl/curl.h>
#include <fstream>
#include "cxxopts.hpp"
#include <chrono>
#include <filesystem> // C++17 feature
using namespace std;

#define DO_NOT_APPLY_GAIN 1.0

// Assuming 4 bytes per sample for S32_LE format and mono audio
int bytesPerSample = 4;
short channels = 1;
unsigned int sampleRate = 44100;
int durationInSeconds = 10; // Duration you want to accumulate before sending
int targetBytes = sampleRate * durationInSeconds * bytesPerSample * channels;
int rc;
float audio_gain = DO_NOT_APPLY_GAIN;
bool save_to_local_file = false;
bool waitting = false;

struct MemoryStruct {
    char *memory;
    size_t size;
};


template <typename T>
class SafeQueue
{
private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cond;

public:
    void push(T value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(value);
        cond.notify_one();
    }

    T pop()
    {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this]
                  { return !queue.empty(); });
        T value = queue.front();
        queue.pop();
        return value;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }
};
SafeQueue<std::vector<char>> audioQueue;

void recordAudio(snd_pcm_t *capture_handle, snd_pcm_uframes_t period_size)
{
    std::vector<char> buffer(period_size * bytesPerSample);
    std::vector<char> accumulatedBuffer;

    while (true)
    {
        rc = snd_pcm_readi(capture_handle, buffer.data(), period_size);
        if (rc == -EPIPE)
        {
            // Handle overrun
            std::cerr << "Overrun occurred" << std::endl;
            snd_pcm_prepare(capture_handle);
        }
        else if (rc < 0)
        {
            std::cerr << "Error from read: " << snd_strerror(rc) << std::endl;
            break; // Exit the loop on error
        }
        else if (rc != (int)period_size)
        {
            std::cerr << "Short read, read " << rc << " frames" << std::endl;
        }
        else
        {
            // Append the captured data to the accumulated buffer
            accumulatedBuffer.insert(accumulatedBuffer.end(), buffer.begin(), buffer.begin() + rc * bytesPerSample * channels);

            if (accumulatedBuffer.size() >= targetBytes)
            {
                audioQueue.push(accumulatedBuffer);
                accumulatedBuffer.clear();
            }
        }
        if(waitting){            
            std::cout << "waitting..." << std::endl;
            sleep(1);
        }
    }
}

void createWavHeader(std::vector<char> &header, int bitsPerSample, int dataSize)
{
    // "RIFF" chunk descriptor
    header.insert(header.end(), {'R', 'I', 'F', 'F'});

    // Chunk size: 4 + (8 + SubChunk1Size) + (8 + SubChunk2Size)
    int chunkSize = 36 + dataSize;
    auto chunkSizeBytes = reinterpret_cast<const char *>(&chunkSize);
    header.insert(header.end(), chunkSizeBytes, chunkSizeBytes + 4);

    // Format
    header.insert(header.end(), {'W', 'A', 'V', 'E'});

    // "fmt " sub-chunk
    header.insert(header.end(), {'f', 'm', 't', ' '});

    // Sub-chunk 1 size (16 for PCM)
    int subchunk1Size = 16;
    auto subchunk1SizeBytes = reinterpret_cast<const char *>(&subchunk1Size);
    header.insert(header.end(), subchunk1SizeBytes, subchunk1SizeBytes + 4);

    // Audio format (PCM = 1)
    short audioFormat = 1;
    auto audioFormatBytes = reinterpret_cast<const char *>(&audioFormat);
    header.insert(header.end(), audioFormatBytes, audioFormatBytes + 2);

    // Number of channels
    auto channelsBytes = reinterpret_cast<const char *>(&channels);
    header.insert(header.end(), channelsBytes, channelsBytes + 2);

    // Sample rate
    auto sampleRateBytes = reinterpret_cast<const char *>(&sampleRate);
    header.insert(header.end(), sampleRateBytes, sampleRateBytes + 4);

    // Byte rate (SampleRate * NumChannels * BitsPerSample/8)
    int byteRate = sampleRate * channels * bitsPerSample / 8;
    auto byteRateBytes = reinterpret_cast<const char *>(&byteRate);
    header.insert(header.end(), byteRateBytes, byteRateBytes + 4);

    // Block align (NumChannels * BitsPerSample/8)
    short blockAlign = channels * bitsPerSample / 8;
    auto blockAlignBytes = reinterpret_cast<const char *>(&blockAlign);
    header.insert(header.end(), blockAlignBytes, blockAlignBytes + 2);

    // Bits per sample
    auto bitsPerSampleBytes = reinterpret_cast<const char *>(&bitsPerSample);
    header.insert(header.end(), bitsPerSampleBytes, bitsPerSampleBytes + 2);

    // "data" sub-chunk
    header.insert(header.end(), {'d', 'a', 't', 'a'});

    // Sub-chunk 2 size (data size)
    auto dataSizeBytes = reinterpret_cast<const char *>(&dataSize);
    header.insert(header.end(), dataSizeBytes, dataSizeBytes + 4);
}

void saveWavToFile(const std::vector<char> &buffer)
{
    // Generate a timestamp for the filename
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);

    // Convert the timestamp to a string
    std::string timestampStr = std::to_string(timestamp);

    // Create the directory "data" if it doesn't exist
    std::filesystem::create_directory("data");

    // Construct the filename with the timestamp
    std::string filename = "data/" + timestampStr + "_audio.wav";

    // Write the buffer to the file
    std::ofstream outfile(filename, std::ios::binary);
    if (outfile.is_open())
    {
        outfile.write(buffer.data(), buffer.size());
        outfile.close();
    }
}

size_t writeData(void *buffer, size_t size, size_t nmemb, void *user_p) {
    FILE *fp = (FILE *)user_p;
    size_t return_size = fwrite(buffer,size,nmemb,fp);
    return return_size;
}

void sendWavBuffer(const std::vector<char> &buffer)
{
    if (save_to_local_file)
    {
        saveWavToFile(buffer);
    }

    CURL *curl;
    CURLcode res;
    const char *supabaseUrlEnv = getenv("SUPABASE_URL");
    if (!supabaseUrlEnv)
    {
        std::cerr << "Environment variable SUPABASE_URL is not set." << std::endl;
        return;
    }

    std::string url = std::string(supabaseUrlEnv) + "/functions/v1/chat-audio";
    std::string authToken = getenv("AUTH_TOKEN");

    // Initialize CURL
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();  
    if (curl)
    {
        FILE *fp = fopen("output.wav", "wb"); // 打开一个用于写入的文件        
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + authToken).c_str());
        headers = curl_slist_append(headers, "Content-Type: audio/wav");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(buffer.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer.data());
        curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // Enable verbose for testing
        //得到请求结果后的回调函数
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeData);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

        res = curl_easy_perform(curl);
        std::cout << res << std::endl;
        if (res != CURLE_OK)
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;         

        // 清理资源
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // 关闭文件
        fclose(fp); 

        std::cout << "ready to play" << std::endl;

        // 播放wav文件
        system("aplay output.wav");
    }

    curl_global_cleanup();
}

void handleAudioBuffer(snd_pcm_t *capture_handle)
{
    while (true)
    {
        std::vector<char> dataChunk;
        // Accumulate our target bytes
        while (dataChunk.size() < targetBytes)
        {
            std::vector<char> buffer = audioQueue.pop();
            dataChunk.insert(dataChunk.end(), buffer.begin(), buffer.end());
        }

        if (audio_gain != DO_NOT_APPLY_GAIN)
        {
            // Apply volume increase by scaling the audio samples
            for (size_t i = 0; i < dataChunk.size(); i += 2) // Assuming 16-bit (2-byte) audio samples
            {
                // Convert the two bytes to a short (16-bit sample)
                short sample = static_cast<short>((dataChunk[i + 1] << 8) | dataChunk[i]);
                // Scale the sample by the volume factor
                sample = static_cast<short>(std::min(std::max(-32768, static_cast<int>(audio_gain * sample)), 32767));
                // Split the short back into two bytes
                dataChunk[i] = sample & 0xFF;
                dataChunk[i + 1] = (sample >> 8) & 0xFF;
            }
        }

        // Process and send the accumulated data
        if (!dataChunk.empty())
        {
            // set flag to pause recording
            waitting = true;
            // 播放提示wav文件
            system("aplay ding.wav");
            int pauseFlag = snd_pcm_pause(capture_handle, 1);
            std::cout << "audio pause: " << pauseFlag << std::endl;


            // Create the WAV header in memory
            std::vector<char> wavHeader;
            int bitsPerSample = 32;
            int dataSize = dataChunk.size();
            createWavHeader(wavHeader, bitsPerSample, dataSize);

            // Combine the header and the data into a single buffer
            std::vector<char> wavBuffer;
            wavBuffer.reserve(wavHeader.size() + dataSize);
            wavBuffer.insert(wavBuffer.end(), wavHeader.begin(), wavHeader.end());
            wavBuffer.insert(wavBuffer.end(), dataChunk.begin(), dataChunk.end());

            sendWavBuffer(wavBuffer);

            // resume recording
            system("aplay dong.wav");
            int resumeFlag = snd_pcm_pause(capture_handle, 0);
            int dropFlag = snd_pcm_drop(capture_handle);
            snd_pcm_prepare(capture_handle);
            std::cout << "audio resume & drop : " << resumeFlag << dropFlag << std::endl;
            waitting = false;

        }
    }
}

void process_args(int argc, char *argv[])
{
    cxxopts::Options options("main", " - command line options");

    options.add_options()("h,help", "Print help")("s,save", "Save audio to local file")("g,gain", "Microphone gain (increase volume of audio)", cxxopts::value<float>());

    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
        std::cout << options.help() << std::endl;
        exit(0);
    }

    if (result.count("save"))
    {
        std::cout << "Saving audio to local file" << std::endl;
        save_to_local_file = true;
    }

    if (result.count("gain"))
    {
        float gain = result["gain"].as<float>();
        std::cout << "Microphone gain: " << gain << std::endl;
        audio_gain = gain;
    }
}

int main(int argc, char *argv[])
{
    process_args(argc, argv);
    snd_pcm_t *capture_handle;
    snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;

    // Open PCM device for recording
    rc = snd_pcm_open(&capture_handle, "default", SND_PCM_STREAM_CAPTURE, 0);
    assert(rc >= 0);
    if (rc < 0)
    {
        std::cerr << "Unable to open pcm device: " << snd_strerror(rc) << std::endl;
        return 1;
    }

    // Set PCM parameters
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;

    rc = snd_pcm_set_params(capture_handle,
                            format,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            channels,
                            sampleRate,
                            1,       // allow software resampling
                            44100); // desired latency

    if (rc < 0)
    {
        std::cerr << "Setting PCM parameters failed: " << snd_strerror(rc) << std::endl;
        return 1;
    }

    // After calling snd_pcm_set_params, you can query the actual buffer size and period size set by ALSA
    snd_pcm_get_params(capture_handle, &buffer_size, &period_size);
    std::vector<char> buffer(period_size * bytesPerSample);

    // Prepare to use the capture handle
    snd_pcm_prepare(capture_handle);

    // Start the recording thread
    std::thread recordingThread(recordAudio, capture_handle, period_size);

    // Start the sending thread
    std::thread sendingThread(handleAudioBuffer, capture_handle);

    recordingThread.join();
    sendingThread.join();

    // Stop PCM device and drop pending frames
    snd_pcm_drop(capture_handle);

    // Close PCM device
    snd_pcm_close(capture_handle);

    return 0;
}