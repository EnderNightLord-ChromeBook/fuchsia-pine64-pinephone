// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use failure::{format_err, Error};
use fidl_fuchsia_setui::*;

pub fn process_string_mutation(mutation: &Mutation) -> Result<Option<SettingData>, Error> {
    if let Mutation::StringMutationValue(mutation_info) = mutation {
        if mutation_info.operation == StringOperation::Update {
            return Ok(Some(SettingData::StringValue(mutation_info.value.clone())));
        }

        return Ok(None);
    } else {
        return Err(format_err!("invalid error"));
    }
}

pub fn process_account_mutation(mutation: &Mutation) -> Result<Option<SettingData>, Error> {
    if let Mutation::AccountMutationValue(mutation_info) = mutation {
        if let Some(operation) = mutation_info.operation {
            if operation == AccountOperation::SetLoginOverride {
                return Ok(Some(SettingData::Account(AccountSettings {
                    mode: mutation_info.login_override,
                })));
            }
        }

        return Ok(None);
    } else {
        return Err(format_err!("invalid error"));
    }
}

pub fn should_sync_account_mutation(mutation: &Mutation) -> bool {
    if let Mutation::AccountMutationValue(mutation_info) = mutation {
        if let Some(operation) = mutation_info.operation {
            return operation == AccountOperation::SetLoginOverride;
        }
    }

    return false;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_should_sync_account_mutation() {
        assert!(should_sync_account_mutation(&Mutation::AccountMutationValue(AccountMutation {
            operation: Some(AccountOperation::SetLoginOverride),
            login_override: Some(LoginOverride::AutologinGuest),
        })));
        assert!(!should_sync_account_mutation(&Mutation::AccountMutationValue(AccountMutation {
            operation: None,
            login_override: None,
        })));
    }
}
