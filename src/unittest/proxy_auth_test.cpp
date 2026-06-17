#include "proxy_auth.h"

#include <cassert>
#include <string>
#include <vector>

int main() {
    {
        TqProxyAuthTable auth(std::vector<TqProxyAuthUser>{
            {"alice", "secret-a"},
            {"bob", "secret-b"},
        });
        assert(auth.Enabled());
        assert(auth.Validate("alice", "secret-a"));
        assert(auth.Validate("bob", "secret-b"));
        assert(!auth.Validate("alice", "wrong"));
        assert(!auth.Validate("carol", "secret-a"));
    }

    {
        TqProxyAuthTable auth;
        assert(!auth.Enabled());
        assert(auth.Validate("any", "value"));
    }

    assert(TqConstantTimeEquals("abc", "abc"));
    assert(!TqConstantTimeEquals("abc", "abd"));
    assert(!TqConstantTimeEquals("abc", "ab"));

    {
        std::string decoded;
        assert(TqBase64Decode("YWxpY2U6c2VjcmV0", decoded));
        assert(decoded == "alice:secret");
    }

    {
        std::string user;
        std::string pass;
        assert(TqParseHttpProxyAuthorization("Basic YWxpY2U6c2VjcmV0", user, pass));
        assert(user == "alice");
        assert(pass == "secret");
    }

    {
        std::string user;
        std::string pass;
        assert(TqParseHttpProxyAuthorization("basic YWxpY2U6c2VjcmV0LWE=", user, pass));
        assert(user == "alice");
        assert(pass == "secret-a");
    }

    {
        const std::string request =
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test:443\r\n"
            "Proxy-Authorization: Basic YWxpY2U6c2VjcmV0LWE=\r\n"
            "\r\n";
        TqProxyAuthTable auth(std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
        assert(TqHttpConnectRequestAuthResult(request, auth) == TqHttpConnectAuthResult::Authorized);
        assert(TqHttpConnectRequestAuthorized(request, auth));

        std::string_view value;
        assert(TqFindHttpHeaderValue(request, "Proxy-Authorization", value));
        assert(value == "Basic YWxpY2U6c2VjcmV0LWE=");
    }

    {
        const std::string request =
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test:443\r\n"
            "\r\n";
        TqProxyAuthTable auth(std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
        assert(TqHttpConnectRequestAuthResult(request, auth) == TqHttpConnectAuthResult::MissingHeader);
        assert(!TqHttpConnectRequestAuthorized(request, auth));
    }

    {
        const std::string request =
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test:443\r\n"
            "Proxy-Authorization: Digest abcdef\r\n"
            "\r\n";
        TqProxyAuthTable auth(std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
        assert(TqHttpConnectRequestAuthResult(request, auth) == TqHttpConnectAuthResult::InvalidHeader);
        assert(!TqHttpConnectRequestAuthorized(request, auth));
    }

    {
        const std::string request =
            "CONNECT example.test:443 HTTP/1.1\r\n"
            "Host: example.test:443\r\n"
            "Proxy-Authorization: Basic YWxpY2U6d3Jvbmc=\r\n"
            "\r\n";
        TqProxyAuthTable auth(std::vector<TqProxyAuthUser>{{"alice", "secret-a"}});
        assert(TqHttpConnectRequestAuthResult(request, auth) == TqHttpConnectAuthResult::InvalidCredentials);
        assert(!TqHttpConnectRequestAuthorized(request, auth));
    }

    {
        std::vector<TqProxyAuthUser> users{{"alice", "secret-a"}, {"alice", "secret-b"}};
        std::string err;
        assert(!TqValidateProxyAuthUsers(users, err));
        assert(err.find("duplicate") != std::string::npos);
    }

    return 0;
}
