package("libarchive")
    set_homepage("https://libarchive.org/")
    set_description("Multi-format archive and compression library")
    set_license("BSD-2-Clause")

    add_urls("https://libarchive.org/downloads/libarchive-$(version).tar.gz")
    add_urls("https://github.com/libarchive/libarchive/releases/download/v$(version)/libarchive-$(version).tar.gz")

    add_versions("3.8.9", "f5a6539059cf5e597dbeda37bfa4874b1e8dea063c8d93bf85a2b44af90a5bd4")

    add_deps("cmake")

    add_configs("openssl3", {description = "Enable use of OpenSSL.", default = true, type = "boolean"})
    add_configs("lzma",     {description = "Deprecated, use xz config instead", type = "boolean"})
    add_configs("xz",       {description = "Enable use of XZ/LZMA.", default = true, type = "boolean"})
    add_configs("zlib",     {description = "Enable use of GZIP/ZIP.",default = true, type = "boolean"})
    add_configs("bzip2",    {description = "Enable use of BZIP2.",   default = true, type = "boolean"})
    add_configs("lz4",      {description = "Enable use of LZ4.",     default = true, type = "boolean"})
    add_configs("lzo",      {description = "Enable use of LZO.",     default = true, type = "boolean"})
    add_configs("zstd",     {description = "Enable use of ZSTD.",    default = true, type = "boolean"})

    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt", "ws2_32", "shlwapi", "user32", "crypt32")
    end

    on_load(function (package)
        if package:config("lzma") ~= nil then
            wprint("package(libarchive): config 'lzma' is deprecated, use 'xz' instead")
        end

        if package:config("openssl3") then
            package:add("deps", "openssl3", {configs = {shared = package:config("shared")}})
        end
        if package:config("xz") then
            package:add("deps", "xz 5.8.3")
        end
        if package:config("zlib") then
            package:add("deps", "zlib 1.3.2")
        end
        if package:config("bzip2") then
            package:add("deps", "bzip2")
        end
        if package:config("lz4") then
            package:add("deps", "lz4")
        end
        if package:config("lzo") then
            package:add("deps", "lzo")
        end
        if package:config("zstd") then
            package:add("deps", "zstd")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        local configs = {"-DENABLE_TEST=OFF",
                         "-DENABLE_CAT=OFF",
                         "-DENABLE_TAR=OFF",
                         "-DENABLE_CPIO=OFF",
                         "-DENABLE_PCREPOSIX=OFF",
                         "-DENABLE_LibGCC=OFF",
                         "-DENABLE_LIBGCC=OFF",
                         "-DENABLE_ICONV=OFF",
                         "-DENABLE_ACL=OFF",
                         "-DENABLE_EXPAT=OFF",
                         "-DENABLE_LIBXML2=OFF",
                         "-DENABLE_LIBB2=OFF"}

        table.insert(configs, "-DENABLE_OPENSSL=" .. (package:config("openssl3") and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_LZMA="    .. (package:config("xz")       and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_ZLIB="    .. (package:config("zlib")     and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_BZip2="   .. (package:config("bzip2")    and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_LZ4="     .. (package:config("lz4")      and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_LZO="     .. (package:config("lzo")      and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_ZSTD="    .. (package:config("zstd")     and "ON" or "OFF"))
        table.insert(configs, "-DENABLE_CNG="     .. (package:is_plat("windows") and "ON" or "OFF"))

        table.insert(configs, "-DCMAKE_BUILD_TYPE="  .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))

        if package:is_plat("windows") then
            table.insert(configs, "-DPOSIX_REGEX_LIB=NONE")
        end
        if not package:config("shared") then
            package:add("defines", "LIBARCHIVE_STATIC")
        end
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("archive_version_number", {includes = "archive.h"}))
    end)
