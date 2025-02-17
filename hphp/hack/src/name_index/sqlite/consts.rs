// Copyright (c) Facebook, Inc. and its affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the "hack" directory of this source tree.

use crate::datatypes::*;
use crate::Result;

use oxidized::relative_path::RelativePath;
use rusqlite::{params, Connection, OptionalExtension};

pub fn create_table(connection: &Connection) -> Result<()> {
    let statement = "
        CREATE TABLE IF NOT EXISTS NAMING_CONSTS (
            HASH INTEGER PRIMARY KEY NOT NULL,
            FILE_INFO_ID INTEGER NOT NULL
        );";

    connection.execute(&statement, params![])?;
    Ok(())
}

pub fn insert(
    connection: &Connection,
    file_info_id: i64,
    names: impl Iterator<Item = String>,
) -> Result<()> {
    let insert_statement = "
        INSERT INTO NAMING_CONSTS (
            HASH,
            FILE_INFO_ID
        ) VALUES (
            ?, ?
        );";

    let mut insert_statement = connection.prepare(&insert_statement)?;

    for name in names {
        let hash = convert::name_to_hash(typing_deps_hash::DepType::GConst, &name);

        insert_statement.execute(params![hash, file_info_id])?;
    }
    Ok(())
}

pub fn get_path(connection: &Connection, name: &str) -> Result<Option<RelativePath>> {
    let select_statement = "
        SELECT
            NAMING_FILE_INFO.PATH_PREFIX_TYPE,
            NAMING_FILE_INFO.PATH_SUFFIX
        FROM
            NAMING_CONSTS
        LEFT JOIN
            NAMING_FILE_INFO
        ON
            NAMING_CONSTS.FILE_INFO_ID = NAMING_FILE_INFO.FILE_INFO_ID
        WHERE
            NAMING_CONSTS.HASH = ?
        ";

    let mut select_statement = connection.prepare_cached(&select_statement)?;
    let hash = convert::name_to_hash(typing_deps_hash::DepType::GConst, &name);
    select_statement
        .query_row::<RelativePath, _, _>(params![hash], |row| {
            let prefix: SqlitePrefix = row.get(0)?;
            let suffix: SqlitePathBuf = row.get(1)?;
            Ok(RelativePath::make(prefix.value, suffix.value))
        })
        .optional()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_add_const() {
        let conn = Connection::open_in_memory().unwrap();
        create_table(&conn).unwrap();
        insert(&conn, 123, ["Foo".to_string()].iter().cloned()).unwrap();
    }
}
