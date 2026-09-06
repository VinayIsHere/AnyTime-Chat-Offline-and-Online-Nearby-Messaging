// core/services/AccountService.h
//
// Account creation and settings. Backs GET/POST/PATCH /{t}/api/account.
//
// AD-05: there is no username availability check, because with no server there
// is no global registry to check against. Usernames are display labels and may
// collide; the UI disambiguates with PeerId::shortLabel(). Creating an account
// is what generates the keypair — identity and account are the same act.

#pragma once

#include <optional>
#include <string>

#include "core/domain/Entities.h"
#include "core/domain/Result.h"

namespace anytime {

class IAccountRepository;
class IIdentityStore;
class IClock;

struct CreateAccountRequest {
    std::string username;
    std::string dateOfBirth;  ///< ISO-8601 "YYYY-MM-DD"
    Gender      gender = Gender::PreferNotToSay;
};

struct UpdateAccountRequest {
    std::optional<std::string> username;
    std::optional<Gender>      gender;
    std::optional<bool>        discoverable;
    std::optional<AcceptFrom>  acceptFrom;
};

class AccountService {
public:
    AccountService(IAccountRepository&, IIdentityStore&, IClock&);

    /// nullopt before setup — the UI routes to /welcome.
    Outcome<std::optional<Account>> current();

    /// Generates the identity keypair, derives the PeerId and stores the account.
    /// Validates: username charset and length, and the 13+ age floor.
    Outcome<Account> create(const CreateAccountRequest&);

    Outcome<Account> update(const UpdateAccountRequest&);

    /// Panic wipe — clears the account, the identity and all message data.
    Result wipe();

    /// `[a-z0-9_]{3,20}`. Uniqueness is deliberately NOT checked (AD-05).
    static bool isValidUsername(const std::string&);

    /// Minimum age is 13, matching the welcome screen's stated terms.
    static bool meetsAgeFloor(const std::string& isoDate, Timestamp now);

private:
    IAccountRepository& accounts_;
    IIdentityStore&     identity_;
    IClock&             clock_;
};

}  // namespace anytime
