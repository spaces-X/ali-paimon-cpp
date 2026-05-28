/*
 * Copyright 2026-present Alibaba Inc.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#include "paimon/global_index/tantivy/tantivy_reader_cache.h"

namespace paimon::tantivy {

TantivyReaderCache& TantivyReaderCache::Instance() {
    static TantivyReaderCache inst;
    return inst;
}

}  // namespace paimon::tantivy
