# Elasticlient discovers JsonCpp in external/, then links its imported target
# from the sibling src/ directory. Import it at project scope for both children.
find_package(jsoncpp CONFIG REQUIRED)
