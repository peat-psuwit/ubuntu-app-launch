
####################
# application.conf
####################

configure_file("application.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/application.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/application.conf")

####################
# application-legacy.conf
####################

configure_file("application-legacy.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/application-legacy.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application-legacy.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application-legacy.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/application-legacy.conf")

####################
# application-click.conf
####################

configure_file("application-click.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/application-click.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application-click.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application-click.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/application-click.conf")

####################
# application-snap.conf
####################

configure_file("application-snap.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/application-snap.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application-snap.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application-snap.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/application-snap.conf")

####################
# application-failed.conf
####################

configure_file("application-failed.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/application-failed.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/application-failed.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application-failed.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/application-failed.conf")

####################
# application-logrotate.conf
####################

install(FILES "application-logrotate.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(application-logrotate.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_SOURCE_DIR}/application-logrotate.conf")

####################
# untrusted-helper.conf
####################

configure_file("untrusted-helper.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(untrusted-helper.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper.conf")

####################
# untrusted-helper-type-end.conf
####################

configure_file("untrusted-helper-type-end.conf.in" "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper-type-end.conf" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper-type-end.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(untrusted-helper-type-end.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_BINARY_DIR}/untrusted-helper-type-end.conf")

####################
# untrusted-helper-logrotate.conf
####################

install(FILES "untrusted-helper-logrotate.conf" DESTINATION "${CMAKE_INSTALL_DATADIR}/upstart/sessions")
add_test(untrusted-helper-logrotate.conf.test "${CMAKE_CURRENT_SOURCE_DIR}/test-conffile.sh" "${CMAKE_CURRENT_SOURCE_DIR}/untrusted-helper-logrotate.conf")

