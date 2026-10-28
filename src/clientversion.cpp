// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>

#include <tinyformat.h>
#include <util/string.h>

#include <string>
#include <vector>

using util::Join;

/**
 * Name of client reported in the 'version' message. Report the same name
 * for both bitcoind and bitcoin-qt, to make it harder for attackers to
 * target servers or GUI users specifically.
 */
const std::string UA_NAME("Resatoshi");


#include <bitcoin-build-info.h>
// Generated source commit (including dirty state), or no Git information.
// git archive substitutes this fallback with the exporting commit.
#define GIT_COMMIT_ID "$Format:%H$"

static std::string FormatVersion(int nVersion)
{
    return strprintf("%d.%d.%d", nVersion / 10000, (nVersion / 100) % 100, nVersion % 100);
}

std::string FormatFullVersion()
{
#ifdef BUILD_GIT_COMMIT
    const std::string source{BUILD_GIT_COMMIT};
#else
    // git archive substitutes the exporting commit; an unexpanded placeholder
    // means provenance is unknown, never the upstream archive's old commit.
    const std::string archive_commit{GIT_COMMIT_ID};
    const std::string source{archive_commit.starts_with("$Format:") ? "unknown" : archive_commit};
#endif
    // Upstream release tools parse the first whitespace-delimited version
    // token, including its commit and optional trailing -dirty marker.
    const std::string revision{source == "unknown" ? source : "g" + source};
    return "v" RESATOSHI_RELEASE "-" + revision + " (Bitcoin Core base " CLIENT_VERSION_STRING ")";
}

/**
 * Format the subversion field according to BIP 14 spec (https://github.com/bitcoin/bips/blob/master/bip-0014.mediawiki)
 */
std::string FormatSubVersion(const std::string& name, int nClientVersion, const std::vector<std::string>& comments)
{
    std::string comments_str;
    if (!comments.empty()) comments_str = strprintf("(%s)", Join(comments, "; "));
    return strprintf("/%s:%s%s/", name, FormatVersion(nClientVersion), comments_str);
}
