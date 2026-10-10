// core/services/AccountService.cpp

#include "core/services/AccountService.h"

#include <algorithm>
#include <cctype>

#include "core/log/Log.h"
#include "core/ports/IClock.h"
#include "core/ports/IIdentityStore.h"
#include "core/ports/IRepositories.h"

namespace anytime {

namespace {

/// Days per month, ignoring leap years; February is handled separately.
constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool isLeapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/// Parses "YYYY-MM-DD" strictly. Returns false for anything else, including
/// dates that do not exist such as 2011-02-31.
bool parseIsoDate(const std::string& iso, int& year, int& month, int& day)
{
    if (iso.size() != 10 || iso[4] != '-' || iso[7] != '-') return false;

    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) {
        if (!std::isdigit(static_cast<unsigned char>(iso[i]))) return false;
    }

    year  = std::stoi(iso.substr(0, 4));
    month = std::stoi(iso.substr(5, 2));
    day   = std::stoi(iso.substr(8, 2));

    if (month < 1 || month > 12 || day < 1) return false;

    int limit = kDaysInMonth[month - 1];
    if (month == 2 && isLeapYear(year)) limit = 29;
    return day <= limit;
}

/// Civil date -> days since the Unix epoch. Howard Hinnant's days_from_civil,
/// which is exact for the whole proleptic Gregorian calendar and avoids pulling
/// a time library into the Qt-free core.
long long daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + static_cast<long long>(doe) - 719468;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

AccountService::AccountService(IAccountRepository& accounts, IIdentityStore& identity,
                               IClock& clock)
    : accounts_(accounts), identity_(identity), clock_(clock)
{
}

bool AccountService::isValidUsername(const std::string& username)
{
    // AD-05: format only. Uniqueness is deliberately NOT checked — with no
    // server there is no registry to check against.
    if (username.size() < kMinUsernameChars || username.size() > kMaxUsernameChars) return false;

    return std::all_of(username.begin(), username.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool AccountService::meetsAgeFloor(const std::string& isoDate, Timestamp now)
{
    int year = 0, month = 0, day = 0;
    if (!parseIsoDate(isoDate, year, month, day)) return false;

    const long long birthDays = daysFromCivil(year, static_cast<unsigned>(month),
                                              static_cast<unsigned>(day));
    const long long todayDays = now / 86'400'000LL;

    if (birthDays > todayDays) return false;  // born in the future

    // Compare against the same calendar date kMinimumAgeYears later, so the
    // check is exact rather than an approximation in days.
    const long long thresholdDays =
        daysFromCivil(year + kMinimumAgeYears, static_cast<unsigned>(month),
                      static_cast<unsigned>(day));
    return todayDays >= thresholdDays;
}

Outcome<std::optional<Account>> AccountService::current()
{
    return accounts_.load();
}

Outcome<Account> AccountService::create(const CreateAccountRequest& request)
{
    auto existing = accounts_.load();
    if (existing.isError()) return existing.result();
    if (existing.value().has_value()) {
        return Outcome<Account>::error(ErrorCode::AlreadyExists,
                                       "An account already exists on this device.");
    }

    if (!isValidUsername(request.username)) {
        return Outcome<Account>::error(
            ErrorCode::InvalidArgument,
            "Usernames are 3-20 characters: lowercase letters, numbers and underscore.");
    }

    const Timestamp now = clock_.now();
    if (!meetsAgeFloor(request.dateOfBirth, now)) {
        return Outcome<Account>::error(ErrorCode::InvalidArgument,
                                       "You must be at least 13 to use AnyTime.");
    }

    // Account creation IS identity creation (AD-05): this generates the keypair
    // from which the PeerId, and therefore the handle, is derived.
    auto identity = identity_.loadOrCreate();
    if (identity.isError()) {
        AT_LOG_ERROR("account", "could not create an identity: {}", identity.result().message());
        return identity.result();
    }

    Account account;
    account.peerId           = identity.value().peerId;
    account.staticPublicKey  = identity.value().staticPublicKey;
    account.signingPublicKey = identity.value().signingPublicKey;
    account.username         = request.username;
    account.dateOfBirth      = request.dateOfBirth;
    account.gender           = request.gender;
    account.discoverable     = true;
    account.acceptFrom       = AcceptFrom::Everyone;
    account.createdAt        = now;

    if (const Result saved = accounts_.save(account); saved.isError()) {
        AT_LOG_ERROR("account", "could not save the new account: {}", saved.message());
        return saved;
    }
    AT_LOG_INFO("account", "account created for peer {}", account.peerId.toHex());
    return account;
}

Outcome<Account> AccountService::update(const UpdateAccountRequest& request)
{
    auto loaded = accounts_.load();
    if (loaded.isError()) return loaded.result();
    if (!loaded.value().has_value()) {
        return Outcome<Account>::error(ErrorCode::NotFound, "No account on this device yet.");
    }

    Account account = *loaded.value();

    if (request.username) {
        if (!isValidUsername(*request.username)) {
            return Outcome<Account>::error(ErrorCode::InvalidArgument, "Invalid username.");
        }
        account.username = *request.username;
    }
    if (request.gender)       account.gender = *request.gender;
    if (request.discoverable) account.discoverable = *request.discoverable;
    if (request.acceptFrom)   account.acceptFrom = *request.acceptFrom;

    if (const Result saved = accounts_.save(account); saved.isError()) {
        AT_LOG_ERROR("account", "could not save account changes: {}", saved.message());
        return saved;
    }
    AT_LOG_INFO("account", "account updated (discoverable: {})", account.discoverable);
    return account;
}

/// TODO(panic-wipe): this clears the account row and the identity key ONLY.
///
/// Every message, chat, contact and outbox row stays in the database, and the
/// at-rest data key that decrypts them stays on disk beside it — so "wipe" today
/// gives a user who triggers it a false assurance, which is worse than not
/// offering it. docs/ARCHITECTURE.md section 10 promises both are cleared.
///
/// The missing pieces already exist and are tested: SqliteStore::wipeAll() and
/// MonocypherDataCipher::wipe(). Finishing this means injecting both here,
/// adding a route, and adding the confirm UI. Deleting the data key alone is the
/// strongest single step — it makes every encrypted row permanently unreadable
/// without having to overwrite anything.
///
/// Deliberately left unwired for now: there is no UI to trigger it yet.
Result AccountService::wipe()
{
    AT_LOG_WARN("account", "wiping account and identity");
    if (const Result r = accounts_.clear(); r.isError()) {
        AT_LOG_ERROR("account", "could not clear the account: {}", r.message());
        return r;
    }
    return identity_.wipe();
}

}  // namespace anytime
