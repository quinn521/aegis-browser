# frozen_string_literal: true

require 'fiddle/import'
require 'json'

module DarwinACL
  extend Fiddle::Importer
  dlload Fiddle.dlopen(nil)

  ACL_TYPE_EXTENDED = 0x0100
  extern 'void *acl_get_fd_np(int, int)'
  extern 'void *acl_to_text(void *, void *)'
  extern 'int acl_free(void *)'

  def self.read(fd)
    acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED)
    if acl.null?
      error = Fiddle.last_error
      # The descriptor exists. ENOENT indicates there is no extended ACL.
      return '' if error == Errno::ENOENT::Errno

      raise SystemCallError.new('acl_get_fd_np', error)
    end

    begin
      text = acl_to_text(acl, nil)
      raise SystemCallError.new('acl_to_text', Fiddle.last_error) if text.null?

      begin
        text.to_s
      ensure
        acl_free(text)
      end
    ensure
      acl_free(acl)
    end
  end
end

File.open(ARGV.fetch(0), File::RDONLY | File::NOFOLLOW | File::NONBLOCK) do |directory|
  info = directory.stat
  raise ArgumentError, 'Expected an output directory' unless info.directory?

  STDOUT.write(JSON.generate(uid: info.uid, mode: info.mode, acl: DarwinACL.read(directory.fileno)))
end
