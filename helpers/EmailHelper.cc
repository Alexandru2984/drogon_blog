#include "EmailHelper.h"
#include "Security.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <trantor/utils/Logger.h>

namespace {

struct Job {
    std::string to;
    std::string subject;
    std::string body;
};

std::mutex                g_mu;
std::condition_variable   g_cv;
std::queue<Job>           g_queue;
std::thread               g_worker;
std::atomic<bool>         g_running{false};

std::string env(const char* key, const char* fallback = "")
{
    const char* v = std::getenv(key);
    return v ? v : fallback;
}

struct UploadCtx {
    const char* data;
    std::size_t remaining;
};

std::size_t readPayload(void* ptr, std::size_t size, std::size_t nmemb, void* userp)
{
    auto* ctx = static_cast<UploadCtx*>(userp);
    if (size == 0 || nmemb == 0 || ctx->remaining == 0) return 0;
    std::size_t want = size * nmemb;
    std::size_t n = std::min(want, ctx->remaining);
    std::memcpy(ptr, ctx->data, n);
    ctx->data      += n;
    ctx->remaining -= n;
    return n;
}

bool sendSync(const std::string& to, const std::string& subject, const std::string& htmlBody)
{
    const std::string server      = env("SMTP_SERVER");
    const std::string username    = env("SMTP_USERNAME");
    const std::string password    = env("SMTP_PASSWORD");
    const std::string fromEmail   = env("SMTP_FROM_EMAIL");
    const std::string fromName    = env("SMTP_FROM_NAME");

    if (server.empty() || fromEmail.empty()) {
        LOG_ERROR << "SMTP not configured; dropping email to " << to;
        return false;
    }

    std::ostringstream payload;
    payload << "To: " << to << "\r\n"
            << "From: " << fromName << " <" << fromEmail << ">\r\n"
            << "Subject: " << subject << "\r\n"
            << "Content-Type: text/html; charset=UTF-8\r\n"
            << "MIME-Version: 1.0\r\n"
            << "\r\n"
            << htmlBody << "\r\n";
    const std::string payloadStr = payload.str();

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    UploadCtx ctx{payloadStr.data(), payloadStr.size()};
    struct curl_slist* rcpts = nullptr;
    rcpts = curl_slist_append(rcpts, to.c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            server.c_str());
    curl_easy_setopt(curl, CURLOPT_USE_SSL,        static_cast<long>(CURLUSESSL_ALL));
    curl_easy_setopt(curl, CURLOPT_USERNAME,       username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD,       password.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM,      fromEmail.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT,      rcpts);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,   readPayload);
    curl_easy_setopt(curl, CURLOPT_READDATA,       &ctx);
    curl_easy_setopt(curl, CURLOPT_UPLOAD,         1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(rcpts);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LOG_ERROR << "SMTP send to " << to << " failed: " << curl_easy_strerror(res);
        return false;
    }
    LOG_INFO << "SMTP send to " << to << " OK";
    return true;
}

void workerLoop()
{
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_queue.empty() || !g_running.load(); });
            if (!g_running.load() && g_queue.empty()) return;
            job = std::move(g_queue.front());
            g_queue.pop();
        }
        sendSync(job.to, job.subject, job.body);
    }
}

void enqueue(Job&& job)
{
    if (!g_running.load()) {
        LOG_WARN << "EmailHelper not running; dropping mail to " << job.to;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_queue.push(std::move(job));
    }
    g_cv.notify_one();
}

} // namespace

void EmailHelper::start()
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) return;
    g_worker = std::thread(workerLoop);
}

void EmailHelper::stop()
{
    if (!g_running.exchange(false)) return;
    g_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
}

std::size_t EmailHelper::queueDepth()
{
    std::lock_guard<std::mutex> lk(g_mu);
    return g_queue.size();
}

std::string EmailHelper::generateToken(std::size_t length)
{
    // libsodium-backed CSPRNG via security::randomToken. The previous
    // implementation seeded std::mt19937 with a single std::random_device
    // call (~32 bits of state), which is *not* cryptographically secure:
    // MT19937 can be backtracked from observed output and brute-forced
    // when seeded from 32 bits. Verification and password-reset tokens
    // were guessable in principle.
    //
    // `length` is interpreted as the desired character count of the
    // URL-safe base64 output. randomToken takes a byte count; 3 bytes
    // produce 4 chars (rounded up), so we ask for ceil(length * 3 / 4)
    // bytes and then trim. The default callers ask for length=32, which
    // gives ~24 bytes ≈ 192 bits of entropy — far above the ~128-bit
    // brute-force floor.
    const std::size_t bytes = (length * 3 + 3) / 4;
    std::string raw = security::randomToken(bytes);
    if (raw.size() > length) raw.resize(length);
    return raw;
}

void EmailHelper::sendVerificationEmail(const std::string& email,
                                        const std::string& username,
                                        const std::string& token)
{
    const std::string link = "https://blog.micutu.com/#/verify-email?token=" + token;
    std::ostringstream body;
    body << "<!DOCTYPE html><html><body style='font-family:Arial,sans-serif;'>"
         << "<div style='max-width:600px;margin:0 auto;padding:20px;'>"
         << "<h2 style='color:#333;'>Welcome to " << env("SMTP_FROM_NAME") << ", " << username << "!</h2>"
         << "<p>Thank you for registering. Please verify your email address by clicking the button below:</p>"
         << "<div style='text-align:center;margin:30px 0;'>"
         << "<a href='" << link << "' style='background-color:#4CAF50;color:white;padding:14px 20px;"
         << "text-decoration:none;border-radius:4px;display:inline-block;'>Verify Email</a>"
         << "</div>"
         << "<p>Or copy and paste this link in your browser:</p>"
         << "<p style='color:#666;word-break:break-all;'>" << link << "</p>"
         << "<p style='color:#999;font-size:12px;margin-top:30px;'>This link will expire in 24 hours.</p>"
         << "</div></body></html>";
    enqueue({email, "Verify your email address", body.str()});
}

void EmailHelper::sendRegistrationAttemptEmail(const std::string& email,
                                               const std::string& existingUsername)
{
    std::ostringstream body;
    body << "<!DOCTYPE html><html><body style='font-family:Arial,sans-serif;'>"
         << "<div style='max-width:600px;margin:0 auto;padding:20px;'>"
         << "<h2 style='color:#333;'>Someone tried to register with your email</h2>"
         << "<p>Hi " << existingUsername << ",</p>"
         << "<p>An account already exists at " << env("SMTP_FROM_NAME")
         << " under this email address. Just now, someone attempted to create a"
         << " <em>new</em> account using the same address.</p>"
         << "<p>If this was you and you forgot you already had an account, you can"
         << " log in or reset your password from the usual links. Otherwise you can"
         << " safely ignore this message — no new account was created.</p>"
         << "<p style='color:#999;font-size:12px;margin-top:30px;'>"
         << "This is an automated notification; no action is required.</p>"
         << "</div></body></html>";
    enqueue({email, "Someone tried to register with your email", body.str()});
}

void EmailHelper::sendLoginThrottleEmail(const std::string& email,
                                         const std::string& username,
                                         int                minutes)
{
    std::ostringstream body;
    body << "<!DOCTYPE html><html><body style='font-family:Arial,sans-serif;'>"
         << "<div style='max-width:600px;margin:0 auto;padding:20px;'>"
         << "<h2 style='color:#333;'>Repeated failed sign-ins on your account</h2>"
         << "<p>Hi " << username << ",</p>"
         << "<p>There have been several failed sign-in attempts on your "
         << env("SMTP_FROM_NAME") << " account, so we have paused sign-ins "
         << "for about " << minutes << " minutes.</p>"
         << "<p><strong>If this was you</strong> — you mistyped your password a "
         << "few times. Wait for the pause to lift, or reset your password from "
         << "the sign-in page.</p>"
         << "<p><strong>If this was not you</strong> — someone is guessing at "
         << "your password. They have not got in: this message means the "
         << "attempts failed. Your password is still worth changing if you use "
         << "it anywhere else, and turning on two-factor authentication in your "
         << "account settings would make a correct guess useless on its own.</p>"
         << "<p style='color:#999;font-size:12px;margin-top:30px;'>"
         << "You will not get another of these until after your next successful "
         << "sign-in.</p>"
         << "</div></body></html>";
    enqueue({email, "Repeated failed sign-ins on your account", body.str()});
}

void EmailHelper::sendPasswordResetEmail(const std::string& email,
                                         const std::string& username,
                                         const std::string& token)
{
    const std::string link = "https://blog.micutu.com/#/reset-password?token=" + token;
    std::ostringstream body;
    body << "<!DOCTYPE html><html><body style='font-family:Arial,sans-serif;'>"
         << "<div style='max-width:600px;margin:0 auto;padding:20px;'>"
         << "<h2 style='color:#333;'>Password Reset Request</h2>"
         << "<p>Hi " << username << ",</p>"
         << "<p>You requested to reset your password. Click the button below to continue:</p>"
         << "<div style='text-align:center;margin:30px 0;'>"
         << "<a href='" << link << "' style='background-color:#2196F3;color:white;padding:14px 20px;"
         << "text-decoration:none;border-radius:4px;display:inline-block;'>Reset Password</a>"
         << "</div>"
         << "<p>Or copy and paste this link in your browser:</p>"
         << "<p style='color:#666;word-break:break-all;'>" << link << "</p>"
         << "<p style='color:#999;font-size:12px;margin-top:30px;'>This link will expire in 1 hour.</p>"
         << "<p style='color:#999;font-size:12px;'>If you didn't request this, please ignore this email.</p>"
         << "</div></body></html>";
    enqueue({email, "Reset your password", body.str()});
}
