color_management_protocol_c = custom_target(
  'color-management-protocol.c',
  input : 'color-management-v1.xml',
  output : 'color-management-v1-protocol.c',
  command : [prog_wl_scanner, wl_scanner_arg, '@INPUT@', '@OUTPUT@'],
)

color_management_client_protocol_h = custom_target(
  'color-management-client-protocol.h',
  input : 'color-management-v1.xml',
  output : 'color-management-v1-client-protocol.h',
  command : [prog_wl_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
