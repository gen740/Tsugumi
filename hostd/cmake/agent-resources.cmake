# Host state and Agent execution live in the host service, independently of project
# lifetimes. Frontends use the separate agent.fbs and host.fbs contracts.
set(_agent_prompts "")
foreach(prompt system-prompt guide-prompt demo-skill review-skill rerun-compare-skill helpers-skill project-files)
  string(REPLACE "-" "_" symbol ${prompt})
  set(output ${CMAKE_CURRENT_BINARY_DIR}/agent_${symbol}.cc)
  set(prompt_options "")
  if(prompt STREQUAL "system-prompt")
    set(prompt_options "-DSTRIP_LINE=<!-- End of shared MCP rules -->")
  endif()
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} ${prompt_options} -DINPUT=${PROJECT_SOURCE_DIR}/hostd/agent/Shared/${prompt}.md
      -DOUTPUT=${output} -DSYMBOL=tsugumi_agent_${symbol}
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/hostd/agent/Shared/${prompt}.md ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    VERBATIM)
  list(APPEND _agent_prompts ${output})
endforeach()
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/agent_settings.cc
  COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/hostd/agent/Shared/agent.json
    -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/agent_settings.cc -DSYMBOL=tsugumi_agent_settings
    -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${PROJECT_SOURCE_DIR}/hostd/agent/Shared/agent.json ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
foreach(language ja en)
  if(language STREQUAL "en")
    set(script guide-script.en.json)
  else()
    set(script guide-script.json)
  endif()
  set(output ${CMAKE_CURRENT_BINARY_DIR}/guide_script_${language}.cc)
  add_custom_command(OUTPUT ${output}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${PROJECT_SOURCE_DIR}/hostd/agent/Shared/${script}
      -DOUTPUT=${output} -DSYMBOL=tsugumi_guide_script_${language}
      -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${PROJECT_SOURCE_DIR}/hostd/agent/Shared/${script} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake VERBATIM)
  list(APPEND _agent_prompts ${output})
endforeach()
