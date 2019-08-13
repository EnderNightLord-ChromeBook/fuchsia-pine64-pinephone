use failure::Fail;
use serde::de;
use serde_derive::{Deserialize, Serialize};
use serde_json::{Map, Value};
use std::fmt;
use url;

/// The in-memory representation of a binary Component Manifest JSON file.
/// This has a 1-1 mapping with the FIDL [`ComponentDecl`] table, which has
/// more complete documentation.
///
/// [`ComponentDecl`]: ../../fidl_fuchsia_sys2/struct.ComponentDecl.html
#[derive(Serialize, Deserialize, Debug, Default)]
pub struct Document {
    /// Program information.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub program: Option<Map<String, Value>>,
    /// Used capabilities.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub uses: Option<Vec<Use>>,
    /// Exposed capabilities.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub exposes: Option<Vec<Expose>>,
    /// Offered capabilities.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub offers: Option<Vec<Offer>>,
    /// Child components.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub children: Option<Vec<Child>>,
    /// Collection declarations.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub collections: Option<Vec<Collection>>,
    /// Storage capability declarations.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub storage: Option<Vec<Storage>>,
    /// Freeform dictionary containing third-party metadata.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub facets: Option<Map<String, Value>>,
}

/// A name that can refer to a component, collection, or other entity in the
/// Component Manifest.
#[derive(Serialize, Clone, Debug)]
pub struct Name(String);

/// A filesystem path.
#[derive(Serialize, Clone, Debug, PartialEq, Eq)]
pub struct Path(String);

/// A component URL. The URL is validated, but represented as a string to avoid
/// normalization and retain the original representation.
#[derive(Serialize, Clone, Debug)]
pub struct Url(String);

/// A component instance's startup mode.
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "snake_case")]
pub enum StartupMode {
    /// Start the component instance only if another component instance binds to
    /// it.
    Lazy,
    /// Start the component instance as soon as its parent starts.
    Eager,
}

/// The duration of child components in a collection.
#[derive(Serialize, Deserialize, Clone, Debug)]
#[serde(rename_all = "snake_case")]
pub enum Durability {
    /// The instance exists until it is explicitly destroyed.
    Persistent,
    /// The instance exists until its containing realm is stopped or it is
    /// explicitly destroyed.
    Transient,
}

/// A child component. See [`ChildDecl`].
///
/// [`ChildDecl`]: ../../fidl_fuchsia_sys2/struct.ChildDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct Child {
    pub name: Name,
    pub url: Url,
    pub startup: StartupMode,
}

/// A component collection. See [`CollectionDecl`].
///
/// [`CollectionDecl`]: ../../fidl_fuchsia_sys2/struct.CollectionDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct Collection {
    pub name: Name,
    pub durability: Durability,
}

/// A storage capability. See [`StorageDecl`].
///
/// [`StorageDecl`]: ../../fidl_fuchsia_sys2/struct.StorageDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct Storage {
    pub name: Name,
    pub source_path: Path,
    pub source: Ref,
}

/// Used capability. See [`UseDecl`].
///
/// [`UseDecl`]: ../../fidl_fuchsia_sys2/enum.UseDecl.html
#[derive(Serialize, Deserialize, Debug)]
#[serde(rename_all = "snake_case")]
pub enum Use {
    /// Used service capability.
    Service(UseService),
    /// Used legacy service capability.
    LegacyService(UseLegacyService),
    /// Used directory capability.
    Directory(UseDirectory),
    /// Used storage capability.
    Storage(UseStorage),
}

/// Used service capability. See [`UseServiceDecl`].
///
/// [`UseServiceDecl`]: ../../fidl_fuchsia_sys2/struct.UseServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct UseService {
    /// Used service source.
    pub source: Ref,
    /// Used service source path.
    pub source_path: Path,
    /// Used service target path.
    pub target_path: Path,
}

/// Used legacy service capability. See [`UseLegacyServiceDecl`].
///
/// [`UseLegacyServiceDecl`]: ../../fidl_fuchsia_sys2/struct.UseLegacyServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct UseLegacyService {
    /// Used service source.
    pub source: Ref,
    /// Used service source path.
    pub source_path: Path,
    /// Used service target path.
    pub target_path: Path,
}

/// Used directory capability. See [`UseDirectoryDecl`].
///
/// [`UseDirectoryDecl`]: ../../fidl_fuchsia_sys2/struct.UseDirectoryDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct UseDirectory {
    /// Used directory source.
    pub source: Ref,
    /// Used directory source path.
    pub source_path: Path,
    /// Used directory target path.
    pub target_path: Path,
}

/// Used storage capability. See [`UseStorageDecl`].
///
/// [`UseStorageDecl`]: ../../fidl_fuchsia_sys2/struct.UseStorageDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct UseStorage {
    /// Used storage type.
    #[serde(rename = "type")]
    pub type_: StorageType,
    /// Used storage target path.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub target_path: Option<Path>,
}

/// Exposed capability. See [`ExposeDecl`].
///
/// [`ExposeDecl`]: ../../fidl_fuchsia_sys2/enum.ExposeDecl.html
#[derive(Serialize, Deserialize, Debug)]
#[serde(rename_all = "snake_case")]
pub enum Expose {
    Service(ExposeService),
    LegacyService(ExposeLegacyService),
    Directory(ExposeDirectory),
}

/// Exposed service capability. See [`ExposeServiceDecl`].
///
/// [`ExposeServiceDecl`]: ../../fidl_fuchsia_sys2/struct.ExposeServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct ExposeService {
    pub source: Ref,
    pub source_path: Path,
    pub target_path: Path,
}

/// Exposed legacy service capability. See [`ExposeLegacyServiceDecl`].
///
/// [`ExposeLegacyServiceDecl`]: ../../fidl_fuchsia_sys2/struct.ExposeLegacyServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct ExposeLegacyService {
    pub source: Ref,
    pub source_path: Path,
    pub target_path: Path,
}

/// Exposed directory capability. See [`ExposeDirectoryDecl`].
///
/// [`ExposeDirectoryDecl`]: ../../fidl_fuchsia_sys2/struct.ExposeDirectoryDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct ExposeDirectory {
    pub source: Ref,
    pub source_path: Path,
    pub target_path: Path,
}

/// Offered capability. See [`OfferDecl`].
///
/// [`OfferDecl`]: ../../fidl_fuchsia_sys2/enum.OfferDecl.html
#[derive(Serialize, Deserialize, Debug)]
#[serde(rename_all = "snake_case")]
pub enum Offer {
    Service(OfferService),
    LegacyService(OfferLegacyService),
    Directory(OfferDirectory),
    Storage(OfferStorage),
}

/// Offered service capability. See [`OfferServiceDecl`].
///
/// [`OfferServiceDecl`]: ../../fidl_fuchsia_sys2/struct.OfferServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct OfferService {
    /// Offered capability source component.
    pub source: Ref,
    /// Offered capability source path.
    pub source_path: Path,
    /// Offered capability target.
    pub target: Ref,
    /// Offered capability target path.
    pub target_path: Path,
}

/// Offered legacy service capability. See [`OfferLegacyServiceDecl`].
///
/// [`OfferLegacyServiceDecl`]: ../../fidl_fuchsia_sys2/struct.OfferLegacyServiceDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct OfferLegacyService {
    /// Offered capability source component.
    pub source: Ref,
    /// Offered capability source path.
    pub source_path: Path,
    /// Offered capability target.
    pub target: Ref,
    /// Offered capability target path.
    pub target_path: Path,
}

/// Offered directory capability. See [`OfferDirectoryDecl`].
///
/// [`OfferDirectoryDecl`]: ../../fidl_fuchsia_sys2/struct.OfferDirectoryDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct OfferDirectory {
    /// Offered capability source component.
    pub source: Ref,
    /// Offered capability source path.
    pub source_path: Path,
    /// Offered capability target.
    pub target: Ref,
    /// Offered capability target path.
    pub target_path: Path,
}

/// Offered storage capability. See [`OfferStorageDecl`].
///
/// [`OfferStorageDecl`]: ../../fidl_fuchsia_sys2/struct.OfferStorageDecl.html
#[derive(Serialize, Deserialize, Debug)]
pub struct OfferStorage {
    #[serde(rename = "type")]
    pub type_: StorageType,
    pub source: Ref,
    pub target: Ref,
}

/// The type of storage capability. See [`StorageType`].
///
/// [`StorageType`]: ../../fidl_fuchsia_sys2/enum.StorageType.html
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(rename_all = "snake_case")]
pub enum StorageType {
    /// Mutable storage the component may store its state in.
    Data,
    /// Identical to the `Data` storage type, but subject to eviction.
    Cache,
    /// Storage in which the framework can store metadata for the component instance.
    Meta,
}

/// A reference to a capability relative to this component. See [`Ref`].
///
/// [`Ref`]: ../../fidl_fuchsia_sys2/enum.Ref.html
#[derive(Serialize, Deserialize, Debug, Clone)]
#[serde(rename_all = "snake_case")]
pub enum Ref {
    /// Component's containing realm (parent component).
    Realm(RealmRef),
    /// Component itself.
    #[serde(rename = "self")]
    Self_(SelfRef),
    /// Component's child.
    Child(ChildRef),
    /// Component's collection.
    Collection(CollectionRef),
    /// Component's storage section.
    Storage(StorageRef),
    /// Component framework.
    Framework(FrameworkRef),
}

/// A reference to a component's containing realm. See [`RealmRef`].
///
/// [`RealmRef`]: ../../fidl_fuchsia_sys2/struct.RealmRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct RealmRef {}

/// A reference to the component itself. See [`SelfRef`].
///
/// [`SelfRef`]: ../../fidl_fuchsia_sys2/struct.SelfRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct SelfRef {}

/// A reference to one of the component's child instances. See [`ChildRef`].
///
/// [`ChildRef`]: ../../fidl_fuchsia_sys2/struct.ChildRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct ChildRef {
    pub name: Name,
}

/// A reference to one of the component's collections. See [`CollectionRef`].
///
/// [`CollectionRef`]: ../../fidl_fuchsia_sys2/struct.CollectionRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct CollectionRef {
    pub name: Name,
}

/// A reference to one of the component's storage sections. See [`StorageRef`].
///
/// [`StorageRef`]: ../../fidl_fuchsia_sys2/struct.StorageRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct StorageRef {
    pub name: Name,
}

/// A reference to the component framework. See [`FrameworkRef`].
///
/// [`FrameworkRef`]: ../../fidl_fuchsia_sys2/struct.FrameworkRef.html
#[derive(Serialize, Deserialize, Debug, Clone)]
pub struct FrameworkRef {}

/// The error representing a failed validation of a `Name` string.
#[derive(Debug, Fail)]
pub enum NameValidationError {
    /// The name string is empty or greater than 100 characters in length.
    #[fail(display = "name must be non-empty and no more than 100 characters in length")]
    InvalidLength,
    /// The name string contains illegal characters. See [`Name::new`].
    #[fail(display = "name must only contain alpha-numeric characters or _-.")]
    MalformedName,
}

impl Name {
    /// Creates a `Name` from a `String`, returning an `Err` if the string
    /// fails validation. The string must be non-empty, no more than 100
    /// characters in length, and consist of one or more of the
    /// following characters: `a-z`, `0-9`, `_`, `.`, `-`.
    pub fn new(name: String) -> Result<Self, NameValidationError> {
        if name.is_empty() || name.len() > 100 {
            return Err(NameValidationError::InvalidLength);
        }
        let valid_fn = |c: char| c.is_ascii_alphanumeric() || c == '_' || c == '-' || c == '.';
        if !name.chars().all(valid_fn) {
            return Err(NameValidationError::MalformedName);
        }
        Ok(Self(name))
    }

    pub fn value(&self) -> &str {
        self.0.as_str()
    }
}

impl Into<String> for Name {
    fn into(self) -> String {
        self.0
    }
}

impl<'de> de::Deserialize<'de> for Name {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: de::Deserializer<'de>,
    {
        struct NameVisitor;

        impl<'de> de::Visitor<'de> for NameVisitor {
            type Value = Name;

            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str(
                    "a non-empty string no more than 100 characters in \
                     length, containing only alpha-numeric characters \
                     or [_-.]",
                )
            }

            fn visit_str<E>(self, s: &str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                Name::new(s.to_owned()).map_err(|err| {
                    let msg = match err {
                        NameValidationError::InvalidLength => {
                            "a non-empty name no more than 100 characters in \
                             length"
                        }
                        NameValidationError::MalformedName => {
                            "a name containing only alpha-numeric characters \
                             or [_-.]"
                        }
                    };
                    E::invalid_value(de::Unexpected::Str(s), &msg)
                })
            }
        }
        deserializer.deserialize_string(NameVisitor)
    }
}

/// The error representing a failed validation of a `Path` string.
#[derive(Debug, Fail)]
pub enum PathValidationError {
    /// The path string is empty or greater than 1024 characters in length.
    #[fail(display = "path must be non-empty and no more than 1024 characters in length")]
    InvalidLength,
    /// The path string is malformed. See [`Path::new`].
    #[fail(display = "path is malformed")]
    MalformedPath,
}

impl Path {
    /// Creates a `Path` from a `String`, returning an `Err` if the string
    /// fails validation. The string must be non-empty, no more than 1024
    /// characters in length, start with a leading `/`, and contain no empty
    /// path segments.
    pub fn new(path: String) -> Result<Self, PathValidationError> {
        if path.is_empty() || path.len() > 1024 {
            return Err(PathValidationError::InvalidLength);
        }
        if !path.starts_with('/') {
            return Err(PathValidationError::MalformedPath);
        }
        if !path[1..].split('/').all(|part| !part.is_empty()) {
            return Err(PathValidationError::MalformedPath);
        }
        return Ok(Self(path));
    }

    pub fn value(&self) -> &str {
        self.0.as_str()
    }
}

impl Into<String> for Path {
    fn into(self) -> String {
        self.0
    }
}

impl<'de> de::Deserialize<'de> for Path {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: de::Deserializer<'de>,
    {
        struct PathVisitor;

        impl<'de> de::Visitor<'de> for PathVisitor {
            type Value = Path;

            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str(
                    "a non-empty string no more than 1024 characters \
                     in length, with a leading `/`, and containing no \
                     empty path segments",
                )
            }

            fn visit_str<E>(self, s: &str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                Path::new(s.to_owned()).map_err(|err| {
                    let msg = match err {
                        PathValidationError::InvalidLength => {
                            "a non-empty path no more than 1024 characters in \
                             length"
                        }
                        PathValidationError::MalformedPath => {
                            "a path with leading `/` and non-empty segments"
                        }
                    };
                    E::invalid_value(de::Unexpected::Str(s), &msg)
                })
            }
        }
        deserializer.deserialize_string(PathVisitor)
    }
}

/// The error representing a failed validation of a `Url` string.
#[derive(Debug, Fail)]
pub enum UrlValidationError {
    /// The URL string is empty or greater than 4096 characters in length.
    #[fail(display = "url must be non-empty and no more than 4096 characters")]
    InvalidLength,
    /// The URL string is not a valid URL. See the [`Url::new`].
    #[fail(display = "url is malformed")]
    MalformedUrl,
}

impl Url {
    /// Creates a `Url` from a `String`, returning an `Err` if the string fails
    /// validation. The string must be non-empty, no more than 4096 characters
    /// in length, and be a valid URL. See the [`url`](../../url/index.html) crate.
    pub fn new(url: String) -> Result<Self, UrlValidationError> {
        if url.is_empty() || url.len() > 4096 {
            return Err(UrlValidationError::InvalidLength);
        }
        let parsed_url = url::Url::parse(&url).map_err(|_| UrlValidationError::MalformedUrl)?;
        if parsed_url.cannot_be_a_base() {
            return Err(UrlValidationError::MalformedUrl);
        }
        // Use the unparsed URL string so that the original format is preserved.
        Ok(Self(url))
    }

    pub fn value(&self) -> &str {
        self.0.as_str()
    }
}

impl Into<String> for Url {
    fn into(self) -> String {
        self.0
    }
}

impl<'de> de::Deserialize<'de> for Url {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: de::Deserializer<'de>,
    {
        struct UrlVisitor;

        impl<'de> de::Visitor<'de> for UrlVisitor {
            type Value = Url;

            fn expecting(&self, f: &mut fmt::Formatter) -> fmt::Result {
                f.write_str("a non-empty URL no more than 4096 characters in length")
            }

            fn visit_str<E>(self, s: &str) -> Result<Self::Value, E>
            where
                E: de::Error,
            {
                Url::new(s.to_owned()).map_err(|err| {
                    let msg = match err {
                        UrlValidationError::InvalidLength => {
                            "a non-empty URL no more than 4096 characters in \
                             length"
                        }
                        UrlValidationError::MalformedUrl => "a valid URL",
                    };
                    E::invalid_value(de::Unexpected::Str(s), &msg)
                })
            }
        }
        deserializer.deserialize_string(UrlVisitor)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::{self, json};
    use std::iter::repeat;

    macro_rules! expect_ok {
        ($type_:ty, $($input:tt)+) => {
            assert!(serde_json::from_str::<$type_>(
                &json!($($input)*).to_string()).is_ok());
        };
    }

    macro_rules! expect_err {
        ($type_:ty, $($input:tt)+) => {
            assert!(serde_json::from_str::<$type_>(
                &json!($($input)*).to_string()).is_err());
        };
    }

    #[test]
    fn test_valid_name() {
        expect_ok!(Name, "foo");
        expect_ok!(Name, "foO123._-");
        expect_ok!(Name, repeat("x").take(100).collect::<String>());
    }

    #[test]
    fn test_invalid_name() {
        expect_err!(Name, "");
        expect_err!(Name, "@&%^");
        expect_err!(Name, repeat("x").take(101).collect::<String>());
    }

    #[test]
    fn test_valid_path() {
        expect_ok!(Path, "/foo");
        expect_ok!(Path, "/foo/bar");
        expect_ok!(Path, &format!("/{}", repeat("x").take(1023).collect::<String>()));
    }

    #[test]
    fn test_invalid_path() {
        expect_err!(Path, "");
        expect_err!(Path, "/");
        expect_err!(Path, "foo");
        expect_err!(Path, "foo/");
        expect_err!(Path, "/foo/");
        expect_err!(Path, "/foo//bar");
        expect_err!(Path, &format!("/{}", repeat("x").take(1024).collect::<String>()));
    }

    #[test]
    fn test_valid_url() {
        expect_ok!(Url, "a://foo");
        expect_ok!(Url, &format!("a://{}", repeat("x").take(4092).collect::<String>()));
    }

    #[test]
    fn test_invalid_url() {
        expect_err!(Url, "");
        expect_err!(Url, "foo");
        expect_err!(Url, &format!("a://{}", repeat("x").take(4093).collect::<String>()));
    }

    #[test]
    fn test_name_error_message() {
        let input = r#"
            "foo$"
        "#;
        let err = serde_json::from_str::<Name>(input).expect_err("must fail");
        assert_eq!(
            err.to_string(),
            "invalid value: string \"foo$\", expected a name containing only \
             alpha-numeric characters or [_-.] at line 2 column 18"
        );
        assert_eq!(err.line(), 2);
        assert_eq!(err.column(), 18);
    }

    #[test]
    fn test_path_error_message() {
        let input = r#"
            "foo"
        "#;
        let err = serde_json::from_str::<Path>(input).expect_err("must fail");
        assert_eq!(
            err.to_string(),
            "invalid value: string \"foo\", expected a path with leading `/` \
             and non-empty segments at line 2 column 17"
        );

        assert_eq!(err.line(), 2);
        assert_eq!(err.column(), 17);
    }

    #[test]
    fn test_url_error_message() {
        let input = r#"
            "foo"
        "#;
        let err = serde_json::from_str::<Url>(input).expect_err("must fail");
        assert_eq!(
            err.to_string(),
            "invalid value: string \"foo\", expected a valid URL at line 2 \
             column 17"
        );
        assert_eq!(err.line(), 2);
        assert_eq!(err.column(), 17);
    }
}
