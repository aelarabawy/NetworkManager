<?xml version="1.0" encoding="UTF-8"?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">

  <xsl:output
      method="xml"
      doctype-public="-//OASIS//DTD DocBook XML V4.3//EN"
      doctype-system="http://www.oasis-open.org/docbook/xml/4.3/docbookx.dtd"
      />

  <xsl:param name="date"/>
  <xsl:param name="version"/>

  <xsl:template match="nm-setting-docs">
    <refentry id="nm-settings">
      <refentryinfo>
        <date><xsl:value-of select="$date"/></date>
      </refentryinfo>
      <refmeta>
        <refentrytitle>nm-settings</refentrytitle>
        <manvolnum>5</manvolnum>
        <refmiscinfo class="source">NetworkManager</refmiscinfo>
        <refmiscinfo class="manual">Configuration</refmiscinfo>
        <refmiscinfo class="version"><xsl:value-of select="$version"/></refmiscinfo>
      </refmeta>
      <refnamediv>
        <refname>nm-settings</refname>
        <refpurpose>Description of settings and properties of NetworkManager connection profiles</refpurpose>
      </refnamediv>
      <refsect1>
        <title>DESCRIPTION</title>
        <para>
          NetworkManager is based on a concept of connection profiles, sometimes referred to as
          connections only. These connection profiles contain a network configuration. When
          NetworkManager activates a connection profile on a network device the configuration will
          be applied and an active network connection will be established. Users are free to create
          as many connection profiles as they see fit. Thus they are flexible in having various network
          configurations for different networking needs. The connection profiles are handled by
          NetworkManager via <emphasis>settings service</emphasis> and are exported on D-Bus
          (<emphasis>/org/freedesktop/NetworkManager/Settings/&lt;num&gt;</emphasis> objects).
          The conceptual objects can be described as follows:
          <variablelist>
            <varlistentry>
              <term>Connection (profile)</term>
              <listitem>
                <para>
                  A specific, encapsulated, independent group of settings describing
                  all the configuration required to connect to a specific network.
                  It is referred to by a unique identifier called the UUID. A connection
                  is tied to a one specific device type, but not necessarily a specific
                  hardware device. It is composed of one or more <emphasis>Settings</emphasis>
                  objects.
                </para>
              </listitem>
            </varlistentry>
          </variablelist>
          <variablelist>
            <varlistentry>
              <term>Setting</term>
              <listitem>
                <para>
                  A group of related key/value pairs describing a specific piece of a
                  <emphasis>Connection (profile)</emphasis>. Settings keys and allowed values are
                  described in the tables below. Keys are also reffered to as properties.
                  Developers can find the setting objects and their properties in the libnm-util
                  sources. Look for the <function>class_init</function> functions near the bottom of
                  each setting source file.
                </para>
              </listitem>
            </varlistentry>
          </variablelist>
          <variablelist>
            <para>
              The settings and properties shown in tables below list all available connection
              configuration options. However, note that not all settings are applicable to all
              connection types. NetworkManager provides a command-line tool <emphasis>nmcli</emphasis>
              that allows direct configuration of the settings and properties according to a connection
              profile type. <emphasis>nmcli</emphasis> connection editor has also a built-in
              <emphasis>describe</emphasis> command that can display description of particular settings
              and properties of this page.
            </para>
          </variablelist>
        </para>
        <xsl:apply-templates/>
        <refsect2 id="secrets-flags">
          <title>Secret flag types:</title>
          <para>
            Each secret property in a setting has an associated <emphasis>flags</emphasis> property
            that describes how to handle that secret. The <emphasis>flags</emphasis> property is a bitfield
            that contains zero or more of the following values logically OR-ed together.
          </para>
          <itemizedlist>
            <listitem>
              <para>0x0 (none) - the system is responsible for providing and storing this secret.</para>
            </listitem>
            <listitem>
              <para>0x1 (agent-owned) - a user-session secret agent is responsible for providing and storing
              this secret; when it is required, agents will be asked to provide it.</para>
            </listitem>
            <listitem>
              <para>0x2 (not-saved) - this secret should not be saved but should be requested from the user
              each time it is required. This flag should be used for One-Time-Pad secrets, PIN codes from hardware tokens,
              or if the user simply does not want to save the secret.</para>
            </listitem>
            <listitem>
              <para>0x4 (not-required) - in some situations it cannot be automatically determined that a secret
              is required or not. This flag hints that the secret is not required and should not be requested from the user.</para>
            </listitem>
          </itemizedlist>
        </refsect2>
      </refsect1>
      <refsect1>
        <title>AUTHOR</title>
        <para>
          <author>
            <firstname>NetworkManager developers</firstname>
          </author>
        </para>
      </refsect1>
      <refsect1>
        <title>FILES</title>
        <para>/etc/NetworkManager/system-connections</para>
        <para>or distro plugin-specific location</para>
      </refsect1>
      <refsect1>
        <title>SEE ALSO</title>
        <para>https://wiki.gnome.org/Projects/NetworkManager/ConfigurationSpecification</para>
        <para>NetworkManager(8), nmcli(1), nmcli-examples(5), NetworkManager.conf(5)</para>
      </refsect1>
    </refentry>
  </xsl:template>

  <xsl:template match="setting">
    <table>
      <title><xsl:value-of select="@name"/> setting</title>
      <tgroup cols="4">
        <thead>
          <row>
            <entry>Key Name</entry>
            <entry>Value Type</entry>
            <entry>Default Value</entry>
            <entry>Value Description</entry>
          </row>
        </thead>
        <tbody>
          <xsl:apply-templates/>
        </tbody>
      </tgroup>
    </table>
  </xsl:template>

  <xsl:template match="property">
    <xsl:variable name="setting_name" select="../@name"/>
    <row>
      <entry align="left"><xsl:value-of select="@name"/></entry>
      <entry align="left"><xsl:value-of select="@type"/></entry>
      <entry align="left"><xsl:value-of select="@default"/></entry>
      <entry><xsl:value-of select="@description"/><xsl:if test="@type = 'NMSettingSecretFlags (uint32)'"> (see <xref linkend="secrets-flags"/> for flag values)</xsl:if></entry>
    </row>
  </xsl:template>

</xsl:stylesheet>
