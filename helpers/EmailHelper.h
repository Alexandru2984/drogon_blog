#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <curl/curl.h>
#include <drogon/drogon.h>
#include <cstdlib>
#include <fstream>

class EmailHelper {
private:
    // Load .env file into environment variables
    static void loadEnvFile() {
        static bool loaded = false;
        if (loaded) return;
        loaded = true;
        
        std::ifstream file(".env");
        if (!file.is_open()) return;
        
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == '#') continue;
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;
            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);
            setenv(key.c_str(), value.c_str(), 0);
        }
    }
    
    static std::string getEnv(const char* name, const char* defaultVal = "") {
        loadEnvFile();
        const char* val = std::getenv(name);
        return val ? val : defaultVal;
    }
    
    static std::string smtpServer()   { return getEnv("SMTP_SERVER"); }
    static std::string smtpUsername() { return getEnv("SMTP_USERNAME"); }
    static std::string smtpPassword() { return getEnv("SMTP_PASSWORD"); }
    static std::string fromEmail()    { return getEnv("SMTP_FROM_EMAIL"); }
    static std::string fromName()     { return getEnv("SMTP_FROM_NAME"); }
    
    // Callback for reading email payload
    struct upload_status {
        const char* payload_text;
        size_t bytes_read;
    };
    
    static size_t payload_source(void* ptr, size_t size, size_t nmemb, void* userp) {
        struct upload_status* upload_ctx = (struct upload_status*)userp;
        const char* data = upload_ctx->payload_text + upload_ctx->bytes_read;
        
        if ((size == 0) || (nmemb == 0) || ((size * nmemb) < 1)) {
            return 0;
        }
        
        size_t len = strlen(data);
        if (len > size * nmemb) {
            len = size * nmemb;
        }
        
        if (len) {
            memcpy(ptr, data, len);
            upload_ctx->bytes_read += len;
            return len;
        }
        
        return 0;
    }
    
    // Send email via SMTP
    static bool sendEmail(const std::string& to, const std::string& subject, const std::string& htmlBody) {
        CURL* curl;
        CURLcode res = CURLE_OK;
        struct curl_slist* recipients = NULL;
        
        std::string server = smtpServer();
        std::string username = smtpUsername();
        std::string password = smtpPassword();
        std::string senderEmail = fromEmail();
        std::string senderName = fromName();
        
        // Build email payload
        std::ostringstream payload;
        payload << "To: " << to << "\r\n";
        payload << "From: " << senderName << " <" << senderEmail << ">\r\n";
        payload << "Subject: " << subject << "\r\n";
        payload << "Content-Type: text/html; charset=UTF-8\r\n";
        payload << "\r\n";
        payload << htmlBody << "\r\n";
        
        std::string payloadStr = payload.str();
        
        struct upload_status upload_ctx;
        upload_ctx.payload_text = payloadStr.c_str();
        upload_ctx.bytes_read = 0;
        
        curl = curl_easy_init();
        if (curl) {
            // Set SMTP server
            curl_easy_setopt(curl, CURLOPT_URL, server.c_str());
            
            // Enable TLS
            curl_easy_setopt(curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
            
            // Set username and password
            curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
            curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
            
            // Set sender
            curl_easy_setopt(curl, CURLOPT_MAIL_FROM, senderEmail.c_str());
            
            // Set recipient
            recipients = curl_slist_append(recipients, to.c_str());
            curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
            
            // Set read callback
            curl_easy_setopt(curl, CURLOPT_READFUNCTION, payload_source);
            curl_easy_setopt(curl, CURLOPT_READDATA, &upload_ctx);
            curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
            
            // Send the email
            res = curl_easy_perform(curl);
            
            if (res != CURLE_OK) {
                LOG_ERROR << "Failed to send email to " << to << ": " << curl_easy_strerror(res);
            } else {
                LOG_INFO << "Email sent successfully to: " << to;
            }
            
            // Cleanup
            curl_slist_free_all(recipients);
            curl_easy_cleanup(curl);
        }
        
        return res == CURLE_OK;
    }

public:
    // Generate random token
    static std::string generateToken(size_t length = 32) {
        const char* chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        std::random_device rd;
        std::mt19937 generator(rd());
        std::uniform_int_distribution<> distribution(0, 61);
        
        std::string token;
        for (size_t i = 0; i < length; ++i) {
            token += chars[distribution(generator)];
        }
        return token;
    }
    
    // Send verification email with real SMTP
    static bool sendVerificationEmail(const std::string& email, 
                                     const std::string& username,
                                     const std::string& token) {
        std::string verificationLink = "http://localhost:8090/auth/verify-email?token=" + token;
        
        std::ostringstream htmlBody;
        htmlBody << "<!DOCTYPE html>"
                 << "<html><body style='font-family: Arial, sans-serif;'>"
                 << "<div style='max-width: 600px; margin: 0 auto; padding: 20px;'>"
                 << "<h2 style='color: #333;'>Welcome to " << getEnv("SMTP_FROM_NAME") << ", " << username << "!</h2>"
                 << "<p>Thank you for registering. Please verify your email address by clicking the button below:</p>"
                 << "<div style='text-align: center; margin: 30px 0;'>"
                 << "<a href='" << verificationLink << "' style='background-color: #4CAF50; color: white; padding: 14px 20px; text-decoration: none; border-radius: 4px; display: inline-block;'>Verify Email</a>"
                 << "</div>"
                 << "<p>Or copy and paste this link in your browser:</p>"
                 << "<p style='color: #666; word-break: break-all;'>" << verificationLink << "</p>"
                 << "<p style='color: #999; font-size: 12px; margin-top: 30px;'>This link will expire in 24 hours.</p>"
                 << "</div></body></html>";
        
        return sendEmail(email, "Verify your email address", htmlBody.str());
    }
    
    static bool sendPasswordResetEmail(const std::string& email,
                                       const std::string& username,
                                       const std::string& token) {
        std::string resetLink = "http://localhost:8090/auth/reset-password?token=" + token;
        
        std::ostringstream htmlBody;
        htmlBody << "<!DOCTYPE html>"
                 << "<html><body style='font-family: Arial, sans-serif;'>"
                 << "<div style='max-width: 600px; margin: 0 auto; padding: 20px;'>"
                 << "<h2 style='color: #333;'>Password Reset Request</h2>"
                 << "<p>Hi " << username << ",</p>"
                 << "<p>You requested to reset your password. Click the button below to continue:</p>"
                 << "<div style='text-align: center; margin: 30px 0;'>"
                 << "<a href='" << resetLink << "' style='background-color: #2196F3; color: white; padding: 14px 20px; text-decoration: none; border-radius: 4px; display: inline-block;'>Reset Password</a>"
                 << "</div>"
                 << "<p>Or copy and paste this link in your browser:</p>"
                 << "<p style='color: #666; word-break: break-all;'>" << resetLink << "</p>"
                 << "<p style='color: #999; font-size: 12px; margin-top: 30px;'>This link will expire in 1 hour.</p>"
                 << "<p style='color: #999; font-size: 12px;'>If you didn't request this, please ignore this email.</p>"
                 << "</div></body></html>";
        
        return sendEmail(email, "Reset your password", htmlBody.str());
    }
};
