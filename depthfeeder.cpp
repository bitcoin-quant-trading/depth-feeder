#include <arpa/inet.h>
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "json/reader.h"

static size_t curlWriteFunc(const char *ptr, size_t size, size_t nmemb, std::string *stream)
{
    size_t len = size * nmemb;
    stream->append(ptr, len);
    return len;
}

static std::string curlHttp(const std::string &url, void *postData, const std::string &header, int timeOut)
{
    std::string content;

    CURL *curl = curl_easy_init();
    std::string useragent = "Mozilla/5.0 (Windows NT 6.1; WOW64; rv:13.0) Gecko/20100101 Firefox/13.0.1";

    curl_easy_setopt(curl, CURLOPT_URL, (std::string("https://") + url).c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeOut / 1000);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeOut / 1000);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (postData)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData);

    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteFunc);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &content);
    CURLcode code = curl_easy_perform(curl);
    if (code != CURLE_OK)
        content = std::string("");

    curl_slist_free_all(headers);

    curl_easy_cleanup(curl);

    return content;
}

static long long timestamp()
{
    struct timeval te;
    gettimeofday(&te, NULL);
    long long ms = te.tv_sec * 1000LL + te.tv_usec / 1000; // caculate milliseconds
    return ms;
}

static pthread_mutex_t mutex;
static long long lastTs = 0;
static std::string url;
static int sock_fd;
static struct sockaddr_in addr_dest;
static double ok_ask = 0.0, ok_bid = 0.0;
static bool isHuobi = false;
static long long lastRefTs = 0;

#define DEST_PORT   8300
#define MAX_DEPTH   1024

static void *depthThread(void *arg)
{
    double depthPack[MAX_DEPTH * 4 + 5];

    pthread_detach(pthread_self());

    long long ts = timestamp();
    std::string content = curlHttp(url, NULL, "", 1000);

    pthread_mutex_lock(&mutex);

    Json::Reader reader;
    Json::Value value;
    if ((ts > lastTs) && reader.parse(content, value)) {
        lastTs = ts;
        Json::Value asks = value["asks"];
        Json::Value bids = value["bids"];
        int askNum = asks.size();
        int bidNum = bids.size();
        double ask, bid;
        int n = 0;
        depthPack[n++] = (double)ts;
        depthPack[n++] = (double)askNum;
        if (asks[0][0].asDouble() < asks[askNum - 1][0].asDouble()) {
            ask = asks[0][0].asDouble();
            for (int i = 0; i < askNum; i++) {
                depthPack[n++] = asks[i][0].asDouble();
                depthPack[n++] = asks[i][1].asDouble();
            }
        } else {
            ask = asks[askNum - 1][0].asDouble();
            for (int i = askNum - 1; i >= 0; i--) {
                depthPack[n++] = asks[i][0].asDouble();
                depthPack[n++] = asks[i][1].asDouble();
            }
        }
        bid = bids[0][0].asDouble();
        for (int i = 0; i < bidNum; i++) {
            depthPack[n++] = bids[i][0].asDouble();
            depthPack[n++] = bids[i][1].asDouble();
        }
        if (isHuobi) {
            depthPack[n++] = ok_ask;
            depthPack[n++] = ok_bid;
        }
        sendto(sock_fd, depthPack, sizeof(double) * n, 0, (const sockaddr *)&addr_dest, sizeof(addr_dest));
        if (!isHuobi)
            printf("Depth sent: %lld, %.2lf, %.2lf\n", ts, ask, bid);
        else
            printf("Depth sent: %lld, %.2lf, %.2lf, %.2lf, %.2lf\n", ts, ask, bid, ok_ask, ok_bid);
    }

    pthread_mutex_unlock(&mutex);

    return NULL;
}

static void *refThread(void *arg)
{
    pthread_detach(pthread_self());

    long long ts = timestamp();
    std::string content = curlHttp("www.okcoin.cn/api/v1/ticker.do?symbol=btc_cny", NULL, "", 1000);

    Json::Reader reader;
    Json::Value value;
    if ((ts > lastRefTs) && reader.parse(content, value)) {
        lastRefTs = ts;
        value = value["ticker"];
        if (value.empty())
            return NULL;
        ok_ask = atof(value["sell"].asString().c_str());
        ok_bid = atof(value["buy"].asString().c_str());
    }

    return NULL;
}

static void sigroutine(int signo){
    pthread_t thread;
    switch (signo){
    case SIGALRM:
        pthread_create(&thread, NULL, depthThread, NULL);
        if (isHuobi) {
            pthread_t thread1;
            pthread_create(&thread1, NULL, refThread, NULL);
        }
        break;
    }
}

int main(int argc, const char *argv[])
{
    struct itimerval tmval;

    if (argc < 3) {
        printf("Usage: %s [okcoin|huobi] [dest ip addr]\n", argv[0]);
        return 0;
    }

    if (!strcmp(argv[1], "okcoin")) {
        printf("Selected source: okcoin.cn\n");
        url = "www.okcoin.cn/api/v1/depth.do?symbol=btc_cny";
    } else if (!strcmp(argv[1], "huobi")) {
        printf("Selected source: huobi\n");
        url = "api.huobi.com/staticmarket/depth_btc_json.js";
        isHuobi = true;
    } else {
        printf("Please choose source, okcoin (default) or houbi.\n");
        return 0;
    }

    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&addr_dest, 0, sizeof(addr_dest));
    addr_dest.sin_family = AF_INET;
    addr_dest.sin_addr.s_addr = inet_addr(argv[2]);
    addr_dest.sin_port = htons(DEST_PORT);

    curl_global_init(CURL_GLOBAL_ALL);

    pthread_mutex_init(&mutex, NULL);

    tmval.it_interval.tv_sec = tmval.it_value.tv_sec = 0;
    tmval.it_interval.tv_usec = tmval.it_value.tv_usec = 110 * 1000;
    signal(SIGALRM, sigroutine);
    setitimer(ITIMER_REAL, &tmval, NULL);
    while (1)
        pause();

    pthread_mutex_destroy(&mutex);

    curl_global_cleanup();

    close(sock_fd);
}
