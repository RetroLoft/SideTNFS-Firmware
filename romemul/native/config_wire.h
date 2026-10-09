/* Utility Falcon window is disjoint from native recovery packets. */
static inline uint16_t r12s_config_address(uint16_t address,bool shifted){
    return shifted && address>=0x7e00u && address<=0x7ffeu && !(address&1u)?
        address-0x7e00u:address;
}
