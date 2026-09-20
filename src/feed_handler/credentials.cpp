#include "feed_handler/credentials.h"

#include <cstdlib>

namespace feed_handler {

EnvReader SystemEnv() {
    return [](std::string_view name) -> std::optional<std::string> {
        // getenv needs a NUL-terminated name and a string_view need not be one.
        const char* value = std::getenv(std::string(name).c_str());
        if (value == nullptr) {
            return std::nullopt;
        }
        return std::string(value);
    };
}

std::expected<std::vector<Credential>, std::string> ResolveCredentials(
    std::span<const config::Connection> connections, const EnvReader& env) {
    std::vector<Credential> credentials;
    credentials.reserve(connections.size());
    std::string missing;

    // Reads one variable into `out`; on failure records only its name.
    const auto read = [&](const config::Connection& connection, const std::string& name,
                          std::string& out) {
        std::optional<std::string> value = env(name);
        if (!value || value->empty()) {
            missing += missing.empty() ? "" : ", ";
            missing += "[" + connection.id + "] " + name;
            return;
        }
        out = std::move(*value);
    };

    for (const config::Connection& connection : connections) {
        Credential credential;
        read(connection, connection.api_key_env, credential.key);
        read(connection, connection.api_secret_env, credential.secret);
        credentials.push_back(std::move(credential));
    }
    if (!missing.empty()) {
        return std::unexpected("missing or empty environment variable(s): " + missing);
    }
    return credentials;
}

}  // namespace feed_handler
